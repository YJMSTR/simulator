#!/usr/bin/env python3
import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


def fail(message):
    print(f"mt-15x-state-update-group-proof-report failed: {message}", file=sys.stderr)
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
    return {
        "cycles": cycles,
        "main": main,
    }


def state_update(task):
    return task.get("state_update") or {}


def state_update_group(task):
    return task.get("state_update_group") or {}


def static_cost(task):
    repcut = task.get("repcut") or {}
    cost = repcut.get("candidate_cost")
    if isinstance(cost, int) and cost > 0:
        return cost
    node_kinds = task.get("node_kinds") or {}
    node_count = sum(value for value in node_kinds.values() if isinstance(value, int))
    return max(1, node_count)


def expected_cost(task, counts):
    return static_cost(task) * counts.get(task["cpp_id"], 0)


def clock_key(task):
    clocks = ((task.get("boundary") or {}).get("clock_names") or [])
    return ",".join(clocks) if clocks else "<none>"


def scan_range_key(task, range_size):
    cpp_id = int(task["cpp_id"])
    begin = (cpp_id // range_size) * range_size
    return f"{begin}-{begin + range_size - 1}"


def runtime_group_key(task, range_size):
    return (
        str(task.get("active_word")),
        scan_range_key(task, range_size),
        clock_key(task),
    )


def target_key(task):
    names = state_update(task).get("state_target_names") or []
    return names[0] if len(names) == 1 else ",".join(names)


def row_for_task(task, counts):
    su = state_update(task)
    group = state_update_group(task)
    return {
        "cpp_id": task["cpp_id"],
        "active_count": counts.get(task["cpp_id"], 0),
        "static_cost": static_cost(task),
        "expected_active_cost": expected_cost(task, counts),
        "active_word": task.get("active_word"),
        "state_target_names": su.get("state_target_names", []),
        "candidate_kind": su.get("candidate_kind", ""),
        "rhs_timing_class": su.get("rhs_timing_class", ""),
        "rhs_timing_evidence": su.get("rhs_timing_evidence", ""),
        "block_reasons": su.get("block_reasons", []),
        "local_safe_candidate": group.get("local_safe_candidate", False),
        "runtime_safe_candidate": group.get("runtime_safe_candidate", False),
        "target_writer_count": group.get("target_writer_count", 0),
        "target_multi_target_writer_count": group.get("target_multi_target_writer_count", 0),
        "target_writer_universe_complete": group.get("target_writer_universe_complete", True),
        "target_writer_cpp_ids": group.get("target_writer_cpp_ids", []),
        "target_writer_conflict_kind": group.get("target_writer_conflict_kind", ""),
        "target_writer_proof": group.get("target_writer_proof", ""),
        "runtime_block_reasons": group.get("runtime_block_reasons", []),
    }


def top_task_rows(tasks, counts, limit):
    rows = [row_for_task(task, counts) for task in tasks]
    rows.sort(key=lambda row: row["expected_active_cost"], reverse=True)
    return rows[:limit]


def summarize(schedule, profile_text, args):
    tasks = schedule.get("tasks")
    expect(isinstance(tasks, list) and tasks, "schedule JSON has no tasks array")
    counts = parse_task_counts(profile_text)
    profile = parse_profile_summary(profile_text)

    state_tasks = [task for task in tasks if state_update(task).get("has_state_update")]
    local_safe_tasks = [task for task in state_tasks if state_update_group(task).get("local_safe_candidate")]
    runtime_safe_tasks = [task for task in state_tasks if state_update_group(task).get("runtime_safe_candidate")]
    target_writer_ids = defaultdict(set)
    target_multi_target_writer_ids = defaultdict(set)
    for task in state_tasks:
        targets = state_update(task).get("state_target_names") or []
        for target in targets:
            target_writer_ids[target].add(task["cpp_id"])
            if len(targets) > 1:
                target_multi_target_writer_ids[target].add(task["cpp_id"])

    def overlaps_multi_target_writer(task):
        targets = state_update(task).get("state_target_names") or []
        return len(targets) == 1 and bool(target_multi_target_writer_ids.get(targets[0]))

    corrected_runtime_safe_tasks = [
        task for task in runtime_safe_tasks
        if not overlaps_multi_target_writer(task)
    ]
    runtime_safe_targets_overlapping_multi_target = sorted({
        state_update(task).get("state_target_names", [""])[0]
        for task in runtime_safe_tasks
        if overlaps_multi_target_writer(task)
    })
    runtime_safe_tasks_overlapping_multi_target = [
        task for task in runtime_safe_tasks
        if overlaps_multi_target_writer(task)
    ]
    duplicate_blocked_local_safe_tasks = [
        task for task in local_safe_tasks
        if "target_multi_writer_unproven" in state_update_group(task).get("runtime_block_reasons", [])
    ]

    total_expected = sum(expected_cost(task, counts) for task in tasks)
    state_expected = sum(expected_cost(task, counts) for task in state_tasks)
    local_safe_expected = sum(expected_cost(task, counts) for task in local_safe_tasks)
    runtime_safe_expected = sum(expected_cost(task, counts) for task in runtime_safe_tasks)
    corrected_runtime_safe_expected = sum(expected_cost(task, counts) for task in corrected_runtime_safe_tasks)
    runtime_safe_overlap_expected = sum(expected_cost(task, counts) for task in runtime_safe_tasks_overlapping_multi_target)
    duplicate_blocked_local_safe_expected = sum(expected_cost(task, counts) for task in duplicate_blocked_local_safe_tasks)

    writer_count_hist = Counter()
    local_safe_writer_count_hist = Counter()
    runtime_block_cost_by_reason = Counter()
    runtime_block_task_count_by_reason = Counter()
    target_rows = {}
    target_writer_counts = {}
    multi_writer_mutual_exclusion_proofs = Counter()

    for task in state_tasks:
        expected = expected_cost(task, counts)
        group = state_update_group(task)
        writer_count = int(group.get("target_writer_count", 0))
        writer_count_hist[writer_count] += 1
        if group.get("local_safe_candidate"):
            local_safe_writer_count_hist[writer_count] += 1
        if group.get("target_writer_proof") not in ("", "none", "target_unique_writer"):
            multi_writer_mutual_exclusion_proofs[group.get("target_writer_proof")] += 1
        for reason in group.get("runtime_block_reasons") or []:
            runtime_block_cost_by_reason[reason] += expected
            runtime_block_task_count_by_reason[reason] += 1
        for target in state_update(task).get("state_target_names") or []:
            row = target_rows.setdefault(target, {
                "target": target,
                "writer_count": 0,
                "multi_target_writer_count": 0,
                "writer_cpp_ids": [],
                "state_update_expected_active_cost": 0,
                "local_safe_expected_active_cost": 0,
                "runtime_safe_expected_active_cost": 0,
                "corrected_runtime_safe_expected_active_cost": 0,
                "duplicate_writer_blocked_expected_active_cost": 0,
            })
            row["state_update_expected_active_cost"] += expected
            if group.get("local_safe_candidate"):
                row["local_safe_expected_active_cost"] += expected
            if group.get("runtime_safe_candidate"):
                row["runtime_safe_expected_active_cost"] += expected
                if not overlaps_multi_target_writer(task):
                    row["corrected_runtime_safe_expected_active_cost"] += expected
            if "target_multi_writer_unproven" in group.get("runtime_block_reasons", []):
                row["duplicate_writer_blocked_expected_active_cost"] += expected
            target_writer_counts[target] = max(target_writer_counts.get(target, 0), writer_count)

    universe_writer_counts = {target: len(writer_ids) for target, writer_ids in target_writer_ids.items()}
    for target, row in target_rows.items():
        row["writer_count"] = universe_writer_counts.get(target, 0)
        row["multi_target_writer_count"] = len(target_multi_target_writer_ids.get(target, set()))
        row["writer_cpp_ids"] = sorted(target_writer_ids.get(target, set()))[:16]
    unique_writer_target_count = sum(1 for value in universe_writer_counts.values() if value == 1)
    multi_writer_target_count = sum(1 for value in universe_writer_counts.values() if value > 1)

    group_rows = defaultdict(lambda: {
        "task_count": 0,
        "expected_active_cost": 0,
        "local_safe_expected_active_cost": 0,
        "runtime_safe_expected_active_cost": 0,
        "corrected_runtime_safe_expected_active_cost": 0,
        "duplicate_writer_blocked_expected_active_cost": 0,
        "cpp_id_min": None,
        "cpp_id_max": None,
    })
    for task in state_tasks:
        key = runtime_group_key(task, args.scan_range_size)
        row = group_rows[key]
        expected = expected_cost(task, counts)
        row["task_count"] += 1
        row["expected_active_cost"] += expected
        if state_update_group(task).get("local_safe_candidate"):
            row["local_safe_expected_active_cost"] += expected
        if state_update_group(task).get("runtime_safe_candidate"):
            row["runtime_safe_expected_active_cost"] += expected
            if not overlaps_multi_target_writer(task):
                row["corrected_runtime_safe_expected_active_cost"] += expected
        if "target_multi_writer_unproven" in state_update_group(task).get("runtime_block_reasons", []):
            row["duplicate_writer_blocked_expected_active_cost"] += expected
        row["cpp_id_min"] = task["cpp_id"] if row["cpp_id_min"] is None else min(row["cpp_id_min"], task["cpp_id"])
        row["cpp_id_max"] = task["cpp_id"] if row["cpp_id_max"] is None else max(row["cpp_id_max"], task["cpp_id"])

    runtime_group_rows = []
    for (active_word, scan_range, clocks), row in group_rows.items():
        out = dict(row)
        out["active_word"] = active_word
        out["scan_range"] = scan_range
        out["clock_name"] = clocks
        runtime_group_rows.append(out)
    runtime_group_rows.sort(key=lambda row: row["corrected_runtime_safe_expected_active_cost"], reverse=True)

    active_runtime_safe_group_count = sum(
        1 for row in runtime_group_rows
        if row["corrected_runtime_safe_expected_active_cost"] >= args.min_group_cost
    )
    runtime_safe_share_total = corrected_runtime_safe_expected / total_expected if total_expected else 0.0
    runtime_safe_share_state = corrected_runtime_safe_expected / state_expected if state_expected else 0.0
    threshold_met = runtime_safe_share_total >= args.min_total_share and active_runtime_safe_group_count >= 2

    if threshold_met:
        decision = "yes"
        reason = "runtime-safe state-update cost meets threshold and spans at least two independent groups"
        next_bottleneck = "implementation planning for minimal NBA-style runtime commit"
    elif corrected_runtime_safe_expected > 0:
        decision = "no"
        reason = "corrected runtime-safe cost exists but does not meet the 5% total expected active cost threshold"
        next_bottleneck = "16X active-frequency weighted batch formation / bounded RepCut-lite"
    else:
        decision = "no"
        reason = "no local-safe cost remains runtime-safe after repaired writer-universe proof"
        next_bottleneck = "16X active-frequency weighted batch formation / bounded RepCut-lite"

    duplicate_targets = [
        row for row in target_rows.values()
        if row["writer_count"] > 1 and row["local_safe_expected_active_cost"] > 0
    ]
    duplicate_targets.sort(key=lambda row: row["duplicate_writer_blocked_expected_active_cost"], reverse=True)

    result = {
        "format": "gsim.mt-15x-state-update-group-proof.v1",
        "source": {
            "schedule_format": schedule.get("format"),
            "profile_cycles": profile["cycles"],
        },
        "thresholds": {
            "min_total_share": args.min_total_share,
            "min_group_cost": args.min_group_cost,
            "scan_range_size": args.scan_range_size,
        },
        "totals": {
            "task_count": len(tasks),
            "state_update_task_count": len(state_tasks),
            "total_expected_active_cost": total_expected,
            "state_update_expected_active_cost": state_expected,
            "local_safe_task_count": len(local_safe_tasks),
            "local_safe_expected_active_cost": local_safe_expected,
            "runtime_safe_task_count": len(runtime_safe_tasks),
            "runtime_safe_expected_active_cost": runtime_safe_expected,
            "corrected_runtime_safe_task_count": len(corrected_runtime_safe_tasks),
            "corrected_runtime_safe_expected_active_cost": corrected_runtime_safe_expected,
            "runtime_safe_targets_overlapping_multi_target": len(runtime_safe_targets_overlapping_multi_target),
            "runtime_safe_tasks_overlapping_multi_target": len(runtime_safe_tasks_overlapping_multi_target),
            "runtime_safe_overlapping_multi_target_expected_active_cost": runtime_safe_overlap_expected,
            "duplicate_writer_blocked_local_safe_task_count": len(duplicate_blocked_local_safe_tasks),
            "duplicate_writer_blocked_expected_active_cost": duplicate_blocked_local_safe_expected,
            "runtime_safe_share_of_total_expected_active_cost": runtime_safe_share_total,
            "runtime_safe_share_of_state_update_expected_active_cost": runtime_safe_share_state,
            "unique_writer_target_count": unique_writer_target_count,
            "multi_writer_target_count": multi_writer_target_count,
        },
        "writer_count_histogram": dict(sorted(writer_count_hist.items())),
        "local_safe_writer_count_histogram": dict(sorted(local_safe_writer_count_hist.items())),
        "runtime_block_cost_by_reason": dict(runtime_block_cost_by_reason.most_common()),
        "runtime_block_task_count_by_reason": dict(runtime_block_task_count_by_reason.most_common()),
        "top_duplicate_writer_targets": duplicate_targets[: args.top],
        "top_local_safe_candidates": top_task_rows(local_safe_tasks, counts, args.top),
        "top_runtime_safe_candidates": top_task_rows(runtime_safe_tasks, counts, args.top),
        "top_runtime_safe_overlapping_multi_target_candidates": top_task_rows(runtime_safe_tasks_overlapping_multi_target, counts, args.top),
        "runtime_candidate_groups": runtime_group_rows[: args.top],
        "runtime_safe_targets_overlapping_multi_target_examples": runtime_safe_targets_overlapping_multi_target[: args.top],
        "group_potential": {
            "active_runtime_safe_group_count": active_runtime_safe_group_count,
            "threshold_met": threshold_met,
            "estimated_workers": min(max(active_runtime_safe_group_count, 1), args.max_workers) if runtime_safe_expected > 0 else 0,
            "basis": "groups are keyed by active word, scan range, and clock name; only runtime-safe group cost counts",
        },
        "mutual_exclusion": {
            "implemented": False,
            "proof_counts": dict(multi_writer_mutual_exclusion_proofs),
            "reason": "15X-C only proves globally unique writers; schedule has no stable writer-condition predicate metadata",
        },
        "recommendation": {
            "can_15x_d_implement_minimal_nba_style_parallel_state_commit": decision,
            "reason": reason,
            "next_bottleneck": next_bottleneck,
        },
        "profile": profile,
    }
    return result


def markdown_table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    lines.extend("| " + " | ".join(str(value) for value in row) + " |" for row in rows)
    return "\n".join(lines)


def write_markdown(result, path):
    totals = result["totals"]
    recommendation = result["recommendation"]
    group_potential = result["group_potential"]
    mutual_exclusion = result["mutual_exclusion"]
    lines = [
        "# GSim-MT 15X-C Group State-Update Safety Proof Report",
        "",
        "## Decision",
        "",
        markdown_table(
            ["question", "answer"],
            [
                ["Can 15X-D implement minimal NBA-style parallel state commit?", recommendation["can_15x_d_implement_minimal_nba_style_parallel_state_commit"]],
                ["Why?", recommendation["reason"]],
                ["Next bottleneck", recommendation["next_bottleneck"]],
            ],
        ),
        "",
        "## Expected Active Cost",
        "",
        markdown_table(
            ["metric", "value"],
            [
                ["total_expected_active_cost", totals["total_expected_active_cost"]],
                ["state_update_expected_active_cost", totals["state_update_expected_active_cost"]],
                ["local_safe_expected_active_cost", totals["local_safe_expected_active_cost"]],
                ["runtime_safe_expected_active_cost", totals["runtime_safe_expected_active_cost"]],
                ["corrected_runtime_safe_expected_active_cost", totals["corrected_runtime_safe_expected_active_cost"]],
                ["runtime_safe_overlapping_multi_target_expected_active_cost", totals["runtime_safe_overlapping_multi_target_expected_active_cost"]],
                ["duplicate_writer_blocked_expected_active_cost", totals["duplicate_writer_blocked_expected_active_cost"]],
                ["runtime_safe_share_of_total_expected_active_cost", f"{totals['runtime_safe_share_of_total_expected_active_cost']:.6f}"],
                ["runtime_safe_share_of_state_update_expected_active_cost", f"{totals['runtime_safe_share_of_state_update_expected_active_cost']:.6f}"],
            ],
        ),
        "",
        "## Writer Proof",
        "",
        markdown_table(
            ["metric", "value"],
            [
                ["local_safe_task_count", totals["local_safe_task_count"]],
                ["runtime_safe_task_count", totals["runtime_safe_task_count"]],
                ["corrected_runtime_safe_task_count", totals["corrected_runtime_safe_task_count"]],
                ["runtime_safe_targets_overlapping_multi_target", totals["runtime_safe_targets_overlapping_multi_target"]],
                ["runtime_safe_tasks_overlapping_multi_target", totals["runtime_safe_tasks_overlapping_multi_target"]],
                ["duplicate_writer_blocked_local_safe_task_count", totals["duplicate_writer_blocked_local_safe_task_count"]],
                ["unique_writer_target_count", totals["unique_writer_target_count"]],
                ["multi_writer_target_count", totals["multi_writer_target_count"]],
                ["mutual_exclusion_proof_implemented", mutual_exclusion["implemented"]],
                ["mutual_exclusion_reason", mutual_exclusion["reason"]],
            ],
        ),
        "",
        "## Writer Count Histogram",
        "",
        markdown_table(
            ["writer_count", "state_update_task_count", "local_safe_task_count"],
            [
                [count, task_count, result["local_safe_writer_count_histogram"].get(count, 0)]
                for count, task_count in result["writer_count_histogram"].items()
            ],
        ),
        "",
        "## Runtime Block Cost By Reason",
        "",
        markdown_table(
            ["reason", "expected_active_cost", "task_count"],
            [
                [reason, cost, result["runtime_block_task_count_by_reason"].get(reason, 0)]
                for reason, cost in result["runtime_block_cost_by_reason"].items()
            ],
        ),
        "",
        "## Group Potential",
        "",
        markdown_table(
            ["metric", "value"],
            [
                ["active_runtime_safe_group_count", group_potential["active_runtime_safe_group_count"]],
                ["estimated_workers", group_potential["estimated_workers"]],
                ["threshold_met", group_potential["threshold_met"]],
                ["basis", group_potential["basis"]],
            ],
        ),
        "",
        "## Top Duplicate-Writer Targets",
        "",
        markdown_table(
            ["target", "writer_count", "duplicate_writer_blocked_cost", "local_safe_cost", "writer_cpp_ids"],
            [
                [
                    row["target"],
                    row["writer_count"],
                    row["duplicate_writer_blocked_expected_active_cost"],
                    row["local_safe_expected_active_cost"],
                    ",".join(str(value) for value in row["writer_cpp_ids"]),
                ]
                for row in result["top_duplicate_writer_targets"]
            ],
        ),
        "",
        "## Top Local-Safe Candidates",
        "",
        markdown_table(
            ["cpp_id", "expected_active_cost", "target", "writer_count", "runtime_block_reasons"],
            [
                [
                    row["cpp_id"],
                    row["expected_active_cost"],
                    ",".join(row["state_target_names"]),
                    row["target_writer_count"],
                    ",".join(row["runtime_block_reasons"]),
                ]
                for row in result["top_local_safe_candidates"]
            ],
        ),
        "",
        "## Runtime-Safe Candidates Overlapping Multi-Target Writers",
        "",
        markdown_table(
            ["cpp_id", "expected_active_cost", "target", "writer_count", "writer_cpp_ids"],
            [
                [
                    row["cpp_id"],
                    row["expected_active_cost"],
                    ",".join(row["state_target_names"]),
                    row["target_writer_count"],
                    ",".join(str(value) for value in row["target_writer_cpp_ids"]),
                ]
                for row in result["top_runtime_safe_overlapping_multi_target_candidates"]
            ],
        ),
        "",
        "## Top Runtime-Safe Candidates",
        "",
        markdown_table(
            ["cpp_id", "expected_active_cost", "target", "writer_count", "proof"],
            [
                [
                    row["cpp_id"],
                    row["expected_active_cost"],
                    ",".join(row["state_target_names"]),
                    row["target_writer_count"],
                    row["target_writer_proof"],
                ]
                for row in result["top_runtime_safe_candidates"]
            ],
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
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--min-total-share", type=float, default=0.05)
    parser.add_argument("--min-group-cost", type=int, default=1)
    parser.add_argument("--scan-range-size", type=int, default=64)
    parser.add_argument("--max-workers", type=int, default=8)
    args = parser.parse_args()

    schedule_path = Path(args.schedule_json)
    profile_path = Path(args.profile_log)
    expect(schedule_path.is_file(), f"missing schedule JSON: {schedule_path}")
    expect(profile_path.is_file(), f"missing profile log: {profile_path}")

    with schedule_path.open() as fp:
        schedule = json.load(fp)
    profile_text = profile_path.read_text(errors="replace")
    result = summarize(schedule, profile_text, args)

    out_json = Path(args.out_json)
    out_md = Path(args.out_md)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(result, indent=2, sort_keys=True))
    write_markdown(result, out_md)

    totals = result["totals"]
    decision = result["recommendation"]["can_15x_d_implement_minimal_nba_style_parallel_state_commit"]
    print(
        "local_safe_expected_active_cost="
        f"{totals['local_safe_expected_active_cost']} "
        "runtime_safe_expected_active_cost="
        f"{totals['runtime_safe_expected_active_cost']} "
        "corrected_runtime_safe_expected_active_cost="
        f"{totals['corrected_runtime_safe_expected_active_cost']} "
        "runtime_safe_targets_overlapping_multi_target="
        f"{totals['runtime_safe_targets_overlapping_multi_target']} "
        "duplicate_writer_blocked_expected_active_cost="
        f"{totals['duplicate_writer_blocked_expected_active_cost']} "
        f"decision={decision}"
    )


if __name__ == "__main__":
    main()
