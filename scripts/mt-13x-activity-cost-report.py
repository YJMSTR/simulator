#!/usr/bin/env python3
import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


def fail(message):
    print(f"mt-13x-activity-cost-report failed: {message}", file=sys.stderr)
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

    rejection_match = re.search(r"\[mt-profile\] rejection_reasons .*$", text, re.M)
    rejection = parse_key_values(rejection_match.group(0)) if rejection_match else {}
    batch_match = re.search(r"\[mt-profile\] batch_size_hist=.*$", text, re.M)
    batch = parse_key_values(batch_match.group(0)) if batch_match else {}
    worker_match = re.search(r"\[mt-profile\] effective_worker_count_hist=.*$", text, re.M)
    worker = worker_match.group(0).split("=", 1)[1] if worker_match else ""
    delta_match = re.search(r"\[mt-profile\] activation_delta .*$", text, re.M)
    delta = parse_key_values(delta_match.group(0)) if delta_match else {}
    partition_match = re.search(r"\[mt-profile\] partition_facts .*$", text, re.M)
    partition = parse_key_values(partition_match.group(0)) if partition_match else {}
    return {
        "cycles": cycles,
        "main": main,
        "rejection_reasons": rejection,
        "batch_size_hist": batch,
        "effective_worker_count_hist": worker,
        "activation_delta": delta,
        "partition_facts": partition,
    }


def primary_blocker(task):
    if task.get("task_kind") != "pure_compute":
        reasons = task.get("serial_reasons") or []
        return reasons[0] if reasons else "serial_without_reason"
    repcut = task.get("repcut") or {}
    if repcut.get("repcut_block_reason"):
        return repcut["repcut_block_reason"]
    if task.get("active_fanout"):
        return "batchable_active_fanout"
    return "batchable_pure"


def static_cost(task):
    repcut = task.get("repcut") or {}
    cost = repcut.get("candidate_cost")
    if isinstance(cost, int) and cost > 0:
        return cost
    node_kinds = task.get("node_kinds") or {}
    node_count = sum(value for value in node_kinds.values() if isinstance(value, int))
    return max(1, node_count)


def dependency_pairs(tasks_by_id, counts):
    rows = []
    for task in tasks_by_id.values():
        src_id = task["cpp_id"]
        src_cost = static_cost(task)
        src_count = counts.get(src_id, 0)
        src_expected = src_cost * src_count
        for dst_id in sorted(set(task.get("succ_cpp_ids", [])) | set(task.get("active_fanout", []))):
            dst = tasks_by_id.get(dst_id)
            if not dst:
                continue
            dst_cost = static_cost(dst)
            dst_count = counts.get(dst_id, 0)
            rows.append({
                "from_cpp_id": src_id,
                "to_cpp_id": dst_id,
                "same_active_word": task.get("active_word") == dst.get("active_word"),
                "from_expected_active_cost": src_expected,
                "to_expected_active_cost": dst_cost * dst_count,
                "combined_expected_active_cost": src_expected + dst_cost * dst_count,
                "from_kind": task.get("task_kind"),
                "to_kind": dst.get("task_kind"),
                "to_blocker": primary_blocker(dst),
            })
    rows.sort(key=lambda row: row["combined_expected_active_cost"], reverse=True)
    return rows


def repcut_candidates(tasks, counts, limit):
    candidates = []
    for task in tasks:
        repcut = task.get("repcut") or {}
        count = counts.get(task["cpp_id"], 0)
        cost = static_cost(task)
        expected = cost * count
        if expected <= 0:
            continue
        if task.get("task_kind") == "pure_compute" and repcut.get("repcut_copy_cost", 0) > 0:
            candidates.append({
                "cpp_id": task["cpp_id"],
                "expected_active_cost": expected,
                "active_count": count,
                "static_cost": cost,
                "copy_cost": repcut.get("repcut_copy_cost", 0),
                "fanout": repcut.get("repcut_fanout", 0),
                "role": repcut.get("repcut_role", "none"),
                "blocker": repcut.get("repcut_block_reason") or "eligible",
                "active_fanout": task.get("active_fanout", []),
            })
    candidates.sort(key=lambda row: (row["expected_active_cost"], -row["copy_cost"]), reverse=True)
    return candidates[:limit]


def summarize(schedule, profile_text, top_n):
    tasks = schedule.get("tasks")
    expect(isinstance(tasks, list), "schedule JSON has no tasks array")
    counts = parse_task_counts(profile_text)
    profile = parse_profile_summary(profile_text)
    cycles = profile["cycles"]
    tasks_by_id = {task["cpp_id"]: task for task in tasks}

    rows = []
    blocker_expected = Counter()
    blocker_active = Counter()
    kind_expected = Counter()
    active_word_expected = Counter()
    batchable_expected = 0
    serial_expected = 0
    total_expected = 0

    for task in tasks:
        cpp_id = task["cpp_id"]
        count = counts.get(cpp_id, 0)
        cost = static_cost(task)
        expected = cost * count
        freq = count / cycles
        blocker = primary_blocker(task)
        kind = task.get("task_kind", "unknown")
        row = {
            "cpp_id": cpp_id,
            "active_count": count,
            "active_frequency": freq,
            "static_cost": cost,
            "expected_active_cost": expected,
            "task_kind": kind,
            "blocker": blocker,
            "active_word": task.get("active_word"),
            "active_fanout": task.get("active_fanout", []),
            "succ_cpp_ids": task.get("succ_cpp_ids", []),
            "serial_reasons": task.get("serial_reasons", []),
            "repcut": task.get("repcut", {}),
        }
        rows.append(row)
        total_expected += expected
        blocker_expected[blocker] += expected
        blocker_active[blocker] += count
        kind_expected[kind] += expected
        active_word_expected[str(task.get("active_word"))] += expected
        if kind == "pure_compute":
            batchable_expected += expected
        else:
            serial_expected += expected

    rows.sort(key=lambda row: row["expected_active_cost"], reverse=True)
    pairs = dependency_pairs(tasks_by_id, counts)
    result = {
        "format": "gsim.mt-13x-activity-cost.v1",
        "profile": profile,
        "task_count": len(tasks),
        "active_task_count": sum(1 for row in rows if row["active_count"] > 0),
        "total_expected_active_cost": total_expected,
        "pure_expected_active_cost": batchable_expected,
        "serial_expected_active_cost": serial_expected,
        "top_tasks": rows[:top_n],
        "blocker_expected_active_cost": dict(blocker_expected.most_common()),
        "blocker_active_count": dict(blocker_active.most_common()),
        "task_kind_expected_active_cost": dict(kind_expected.most_common()),
        "active_word_expected_active_cost": dict(active_word_expected.most_common()),
        "top_blocker_pairs": pairs[:top_n],
        "repcut_lite_candidates": repcut_candidates(tasks, counts, top_n),
    }
    return result


def markdown_table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    lines.extend("| " + " | ".join(str(value) for value in row) + " |" for row in rows)
    return "\n".join(lines)


def write_markdown(result, path):
    profile = result["profile"]
    main = profile["main"]
    delta = profile["activation_delta"]
    lines = [
        "# GSim-MT 13X Activity Cost Report",
        "",
        "## Profile Summary",
        "",
        markdown_table(
            ["cycles", "worker_count", "max_worker_count", "true_parallel_batch_count", "skipped_fake_parallel_batch_count", "activation_delta_entries"],
            [[
                profile["cycles"],
                main.get("worker_count", ""),
                main.get("max_worker_count", ""),
                main.get("true_parallel_batch_count", ""),
                main.get("skipped_fake_parallel_batch_count", ""),
                delta.get("entries", ""),
            ]],
        ),
        "",
        "## Expected Active Cost",
        "",
        markdown_table(
            ["metric", "value"],
            [
                ["task_count", result["task_count"]],
                ["active_task_count", result["active_task_count"]],
                ["total_expected_active_cost", result["total_expected_active_cost"]],
                ["pure_expected_active_cost", result["pure_expected_active_cost"]],
                ["serial_expected_active_cost", result["serial_expected_active_cost"]],
            ],
        ),
        "",
        "## Top Tasks",
        "",
        markdown_table(
            ["cpp_id", "active_count", "active_frequency", "static_cost", "expected_active_cost", "task_kind", "blocker", "active_word"],
            [
                [
                    row["cpp_id"],
                    row["active_count"],
                    f"{row['active_frequency']:.6f}",
                    row["static_cost"],
                    row["expected_active_cost"],
                    row["task_kind"],
                    row["blocker"],
                    row["active_word"],
                ]
                for row in result["top_tasks"]
            ],
        ),
        "",
        "## Top Blocker Reasons",
        "",
        markdown_table(
            ["blocker", "expected_active_cost", "active_count"],
            [
                [blocker, cost, result["blocker_active_count"].get(blocker, 0)]
                for blocker, cost in result["blocker_expected_active_cost"].items()
            ],
        ),
        "",
        "## Top Dependency Or Activation Pairs",
        "",
        markdown_table(
            ["from", "to", "same_active_word", "combined_expected_active_cost", "from_kind", "to_kind", "to_blocker"],
            [
                [
                    row["from_cpp_id"],
                    row["to_cpp_id"],
                    row["same_active_word"],
                    row["combined_expected_active_cost"],
                    row["from_kind"],
                    row["to_kind"],
                    row["to_blocker"],
                ]
                for row in result["top_blocker_pairs"]
            ],
        ),
        "",
        "## Bounded RepCut-Lite Candidate Inputs",
        "",
        markdown_table(
            ["cpp_id", "expected_active_cost", "copy_cost", "fanout", "role", "blocker", "active_fanout"],
            [
                [
                    row["cpp_id"],
                    row["expected_active_cost"],
                    row["copy_cost"],
                    row["fanout"],
                    row["role"],
                    row["blocker"],
                    ",".join(str(item) for item in row["active_fanout"]),
                ]
                for row in result["repcut_lite_candidates"]
            ],
        ),
        "",
        "## Raw Profile Lines",
        "",
        "```text",
        "[mt-profile] " + " ".join(f"{key}={value}" for key, value in profile["main"].items()),
        "[mt-profile] rejection_reasons " + " ".join(f"{key}={value}" for key, value in profile["rejection_reasons"].items()),
        "[mt-profile] batch_size_hist " + " ".join(f"{key}={value}" for key, value in profile["batch_size_hist"].items()),
        "[mt-profile] effective_worker_count_hist=" + profile["effective_worker_count_hist"],
        "```",
        "",
    ]
    path.write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--schedule-json", required=True)
    parser.add_argument("--profile-log", required=True)
    parser.add_argument("--out-json", required=True)
    parser.add_argument("--out-md", required=True)
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()

    schedule_path = Path(args.schedule_json)
    profile_path = Path(args.profile_log)
    expect(schedule_path.is_file(), f"missing schedule JSON {schedule_path}")
    expect(profile_path.is_file(), f"missing profile log {profile_path}")

    schedule = json.loads(schedule_path.read_text())
    profile_text = profile_path.read_text(errors="replace")
    result = summarize(schedule, profile_text, args.top)

    out_json = Path(args.out_json)
    out_md = Path(args.out_md)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_md.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(result, indent=2) + "\n")
    write_markdown(result, out_md)
    print(
        f"mt-13x-activity-cost-report ok: tasks={result['task_count']} "
        f"active_tasks={result['active_task_count']} "
        f"total_expected_active_cost={result['total_expected_active_cost']} "
        f"top_blocker={next(iter(result['blocker_expected_active_cost']), 'none')}"
    )


if __name__ == "__main__":
    main()
