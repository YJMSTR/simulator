#!/usr/bin/env python3
import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path


ACTIVE_WIDTH = 8
REQUIRED_BLOCKER_REASONS = [
    "dependency edge",
    "same-active-word hazard",
    "too-small batch",
    "serial boundary",
    "active visibility boundary",
    "RepCut budget",
    "fanout budget",
]


def fail(message):
    print(f"mt-16x-active-frequency-batch-report failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def parse_key_values(line):
    return {key: value.rstrip(",") for key, value in re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", line)}


def parse_task_counts(text):
    match = re.search(r"\[mt-profile\] task_cpp_ids=([^\n]*)", text, re.M)
    expect(match is not None, "profile log has no task_cpp_ids line; rerun with GSIM_MT_PROFILE_TASKS=1")
    counts = {}
    body = match.group(1).strip()
    if not body:
        return counts
    for item in body.split(","):
        if not item.strip():
            continue
        cpp_id, count = item.split(":", 1)
        counts[int(cpp_id)] = int(count)
    return counts


def parse_profile_summary(text):
    line_match = re.search(r"\[mt-profile\] helper_mode=.*$", text, re.M)
    expect(line_match is not None, "profile log has no main mt-profile line")
    main = parse_key_values(line_match.group(0))
    cycles = int(main.get("cycles", "0"))
    expect(cycles > 0, "profile cycles must be positive")
    delta_match = re.search(r"\[mt-profile\] activation_delta .*$", text, re.M)
    worker_match = re.search(r"\[mt-profile\] effective_worker_count_hist=.*$", text, re.M)
    rejection_match = re.search(r"\[mt-profile\] rejection_reasons .*$", text, re.M)
    return {
        "cycles": cycles,
        "main": main,
        "activation_delta": parse_key_values(delta_match.group(0)) if delta_match else {},
        "effective_worker_count_hist": worker_match.group(0).split("=", 1)[1] if worker_match else "",
        "rejection_reasons": parse_key_values(rejection_match.group(0)) if rejection_match else {},
    }


def static_cost(task):
    repcut = task.get("repcut") or {}
    cost = repcut.get("candidate_cost")
    if isinstance(cost, int) and cost > 0:
        return cost
    node_kinds = task.get("node_kinds") or {}
    return max(1, sum(value for value in node_kinds.values() if isinstance(value, int)))


def expected_cost(task, counts):
    return static_cost(task) * counts.get(task["cpp_id"], 0)


def refs(task):
    return set(task.get("pred_cpp_ids") or []) | set(task.get("succ_cpp_ids") or []) | set(task.get("active_fanout") or [])


def has_edge(lhs, rhs):
    lhs_id = lhs["cpp_id"]
    rhs_id = rhs["cpp_id"]
    return rhs_id in refs(lhs) or lhs_id in refs(rhs)


def repcut_budget_reason(task):
    repcut = task.get("repcut") or {}
    reason = repcut.get("repcut_block_reason")
    if reason in {"copy_budget_zero", "copy_budget_exceeded", "disabled"}:
        return "RepCut budget"
    if reason in {"fanout_budget_zero", "fanout_budget_exceeded"}:
        return "fanout budget"
    return None


def pure_blocker(task, tasks_by_id, tasks_by_active_word):
    if task.get("task_kind") != "pure_compute":
        return "serial boundary"
    budget = repcut_budget_reason(task)
    if budget:
        return budget
    cpp_id = task["cpp_id"]
    active_word = task.get("active_word")
    same_word_peers = [
        peer for peer in tasks_by_active_word.get(active_word, [])
        if peer["cpp_id"] != cpp_id
    ]
    if any(has_edge(task, peer) for peer in same_word_peers):
        return "same-active-word hazard"
    if any(tasks_by_id.get(ref, {}).get("task_kind") != "pure_compute" for ref in refs(task) if ref in tasks_by_id):
        return "serial boundary"
    if any(ref // ACTIVE_WIDTH != active_word for ref in task.get("active_fanout") or []):
        return "active visibility boundary"
    if any(ref in tasks_by_id and tasks_by_id[ref].get("task_kind") == "pure_compute" for ref in refs(task)):
        return "dependency edge"
    return "too-small batch"


def task_row(task, counts, tasks_by_id, tasks_by_active_word):
    cpp_id = task["cpp_id"]
    active_count = counts.get(cpp_id, 0)
    cost = static_cost(task)
    return {
        "cpp_id": cpp_id,
        "active_count": active_count,
        "static_cost": cost,
        "expected_active_cost": cost * active_count,
        "active_word": task.get("active_word"),
        "pred_cpp_ids": task.get("pred_cpp_ids") or [],
        "succ_cpp_ids": task.get("succ_cpp_ids") or [],
        "active_fanout": task.get("active_fanout") or [],
        "task_kind": task.get("task_kind"),
        "cannot_join_multi_worker_batch_reason": pure_blocker(task, tasks_by_id, tasks_by_active_word),
        "serial_reasons": task.get("serial_reasons") or [],
        "repcut": task.get("repcut") or {},
    }


def candidate_windows(tasks, counts, max_width, threshold, top_n):
    rows = []
    tasks_by_id = {task["cpp_id"]: task for task in tasks}
    for begin in range(0, len(tasks), ACTIVE_WIDTH):
        word_tasks = tasks[begin:begin + ACTIVE_WIDTH]
        for i in range(len(word_tasks)):
            for j in range(i + 2, min(len(word_tasks), i + max_width) + 1):
                window = word_tasks[i:j]
                if any(task.get("task_kind") != "pure_compute" for task in window):
                    reason = "serial boundary"
                elif any(has_edge(lhs, rhs) for pos, lhs in enumerate(window) for rhs in window[pos + 1:]):
                    reason = "dependency edge"
                else:
                    reason = "candidate"
                expected = sum(expected_cost(task, counts) for task in window)
                if expected < threshold and reason == "candidate":
                    reason = "too-small batch"
                if reason in {"candidate", "too-small batch"} or expected > 0:
                    rows.append({
                        "begin_cpp_id": window[0]["cpp_id"],
                        "end_cpp_id": window[-1]["cpp_id"] + 1,
                        "task_count": len(window),
                        "active_word": window[0].get("active_word"),
                        "expected_active_cost": expected,
                        "static_cost": sum(static_cost(task) for task in window),
                        "reason": reason,
                        "cpp_ids": [task["cpp_id"] for task in window],
                    })
    rows.sort(key=lambda row: (row["reason"] == "candidate", row["expected_active_cost"], row["task_count"]), reverse=True)
    return rows[:top_n]


def summarize(schedule, profile_text, top_n, window_width, window_threshold):
    tasks = schedule.get("tasks")
    expect(isinstance(tasks, list) and tasks, "schedule JSON has no non-empty tasks array")
    counts = parse_task_counts(profile_text)
    profile = parse_profile_summary(profile_text)
    tasks_by_id = {task["cpp_id"]: task for task in tasks}
    tasks_by_active_word = {}
    for task in tasks:
        if task.get("task_kind") == "pure_compute":
            tasks_by_active_word.setdefault(task.get("active_word"), []).append(task)
    rows = [task_row(task, counts, tasks_by_id, tasks_by_active_word) for task in tasks if task.get("task_kind") == "pure_compute"]
    rows.sort(key=lambda row: row["expected_active_cost"], reverse=True)

    blocker_expected = Counter()
    blocker_active = Counter()
    for reason in REQUIRED_BLOCKER_REASONS:
        blocker_expected[reason] = 0
        blocker_active[reason] = 0
    for row in rows:
        reason = row["cannot_join_multi_worker_batch_reason"]
        blocker_expected[reason] += row["expected_active_cost"]
        blocker_active[reason] += row["active_count"]

    return {
        "format": "gsim.mt-16x-active-frequency-batch-report.v1",
        "profile": profile,
        "task_count": len(tasks),
        "pure_task_count": len(rows),
        "active_pure_task_count": sum(1 for row in rows if row["active_count"] > 0),
        "total_pure_expected_active_cost": sum(row["expected_active_cost"] for row in rows),
        "blocker_expected_active_cost": dict(blocker_expected.most_common()),
        "blocker_active_count": dict(blocker_active.most_common()),
        "pure_tasks": rows,
        "top_pure_tasks": rows[:top_n],
        "top_candidate_windows": candidate_windows(tasks, counts, window_width, window_threshold, top_n),
    }


def markdown_table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    lines.extend("| " + " | ".join(str(value) for value in row) + " |" for row in rows)
    return "\n".join(lines)


def write_markdown(result, path):
    profile = result["profile"]
    main = profile["main"]
    delta = profile["activation_delta"]
    lines = [
        "# GSim-MT 16X Active-Frequency Batch Report",
        "",
        "## Profile Summary",
        "",
        markdown_table(
            ["cycles", "worker_count", "max_worker_count", "true_parallel_batch_count", "skipped_fake_parallel_batch_count", "worker_hist", "merge_wall_ns"],
            [[
                profile["cycles"],
                main.get("worker_count", ""),
                main.get("max_worker_count", ""),
                main.get("true_parallel_batch_count", ""),
                main.get("skipped_fake_parallel_batch_count", ""),
                profile["effective_worker_count_hist"],
                main.get("merge_wall_ns", ""),
            ]],
        ),
        "",
        "## Blocker Cost",
        "",
        markdown_table(
            ["reason", "expected_active_cost", "active_count"],
            [
                [reason, cost, result["blocker_active_count"].get(reason, "")]
                for reason, cost in result["blocker_expected_active_cost"].items()
            ],
        ),
        "",
        "## Top Candidate Windows",
        "",
        markdown_table(
            ["begin", "end", "tasks", "active_word", "expected_active_cost", "static_cost", "reason", "cpp_ids"],
            [
                [
                    row["begin_cpp_id"],
                    row["end_cpp_id"],
                    row["task_count"],
                    row["active_word"],
                    row["expected_active_cost"],
                    row["static_cost"],
                    row["reason"],
                    ",".join(str(value) for value in row["cpp_ids"]),
                ]
                for row in result["top_candidate_windows"]
            ],
        ),
        "",
        "## Top Pure Tasks",
        "",
        markdown_table(
            ["cpp_id", "active_count", "static_cost", "expected_active_cost", "active_word", "reason", "pred", "succ"],
            [
                [
                    row["cpp_id"],
                    row["active_count"],
                    row["static_cost"],
                    row["expected_active_cost"],
                    row["active_word"],
                    row["cannot_join_multi_worker_batch_reason"],
                    ",".join(str(value) for value in row["pred_cpp_ids"][:8]),
                    ",".join(str(value) for value in row["succ_cpp_ids"][:8]),
                ]
                for row in result["top_pure_tasks"]
            ],
        ),
        "",
        "## ActivationDelta",
        "",
        markdown_table(
            ["entries", "max_entries_per_worker", "activate_all_count"],
            [[delta.get("entries", ""), delta.get("max_entries_per_worker", ""), delta.get("activate_all_count", "")]],
        ),
        "",
    ]
    path.write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--schedule-json", required=True)
    parser.add_argument("--profile-log", required=True)
    parser.add_argument("--out-json", required=True)
    parser.add_argument("--out-md", required=True)
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--window-width", type=int, default=ACTIVE_WIDTH)
    parser.add_argument("--window-threshold", type=int, default=2)
    args = parser.parse_args()

    schedule_path = Path(args.schedule_json)
    profile_path = Path(args.profile_log)
    expect(schedule_path.is_file(), f"missing schedule JSON {schedule_path}")
    expect(profile_path.is_file(), f"missing profile log {profile_path}")
    result = summarize(json.loads(schedule_path.read_text()), profile_path.read_text(errors="replace"),
                       args.top, args.window_width, args.window_threshold)

    out_json = Path(args.out_json)
    out_md = Path(args.out_md)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_md.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(result, indent=2) + "\n")
    write_markdown(result, out_md)
    print(
        f"mt-16x-active-frequency-batch-report ok: pure_tasks={result['pure_task_count']} "
        f"active_pure_tasks={result['active_pure_task_count']} "
        f"top_blocker={next(iter(result['blocker_expected_active_cost']), 'none')}"
    )


if __name__ == "__main__":
    main()
