#!/usr/bin/env python3
import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


def fail(message):
    print(f"mt-15x-state-update-proof-report failed: {message}", file=sys.stderr)
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
    batch_match = re.search(r"\[mt-profile\] batch_size_hist=.*$", text, re.M)
    worker_match = re.search(r"\[mt-profile\] effective_worker_count_hist=.*$", text, re.M)
    delta_match = re.search(r"\[mt-profile\] activation_delta .*$", text, re.M)
    partition_match = re.search(r"\[mt-profile\] partition_facts .*$", text, re.M)
    return {
        "cycles": cycles,
        "main": main,
        "rejection_reasons": parse_key_values(rejection_match.group(0)) if rejection_match else {},
        "batch_size_hist": parse_key_values(batch_match.group(0)) if batch_match else {},
        "effective_worker_count_hist": worker_match.group(0).split("=", 1)[1] if worker_match else "",
        "activation_delta": parse_key_values(delta_match.group(0)) if delta_match else {},
        "partition_facts": parse_key_values(partition_match.group(0)) if partition_match else {},
    }


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


def state_update(task):
    return task.get("state_update") or {}


def clock_key(task):
    clocks = ((task.get("boundary") or {}).get("clock_names") or [])
    return ",".join(clocks) if clocks else "<none>"


def scan_range_key(task, range_size):
    cpp_id = int(task["cpp_id"])
    begin = (cpp_id // range_size) * range_size
    return f"{begin}-{begin + range_size - 1}"


def candidate_group_key(task, range_size):
    return (
        str(task.get("active_word")),
        scan_range_key(task, range_size),
        clock_key(task),
    )


def top_rows(tasks, counts, limit):
    rows = []
    for task in tasks:
        su = state_update(task)
        rows.append({
            "cpp_id": task["cpp_id"],
            "active_count": counts.get(task["cpp_id"], 0),
            "static_cost": static_cost(task),
            "expected_active_cost": expected_cost(task, counts),
            "active_word": task.get("active_word"),
            "activation_fanout_count": su.get("activation_fanout_count", len(task.get("active_fanout", []))),
            "clock_names": (task.get("boundary") or {}).get("clock_names", []),
            "state_target_names": su.get("state_target_names", []),
            "rhs_timing_class": su.get("rhs_timing_class", ""),
            "rhs_timing_evidence": su.get("rhs_timing_evidence", ""),
            "rhs_reads_state_targets": su.get("rhs_reads_state_targets", []),
            "rhs_reads_same_cycle_target": su.get("rhs_reads_same_cycle_target", False),
            "candidate_kind": su.get("candidate_kind", ""),
            "block_reasons": su.get("block_reasons", []),
            "serial_reasons": task.get("serial_reasons", []),
        })
    rows.sort(key=lambda row: row["expected_active_cost"], reverse=True)
    return rows[:limit]


def summarize(schedule, profile_text, args):
    tasks = schedule.get("tasks")
    expect(isinstance(tasks, list) and tasks, "schedule JSON has no tasks array")
    counts = parse_task_counts(profile_text)
    profile = parse_profile_summary(profile_text)

    state_tasks = [task for task in tasks if state_update(task).get("has_state_update")]
    safe_tasks = [task for task in state_tasks if state_update(task).get("candidate_kind") == "safe_candidate"]
    blocked_tasks = [task for task in state_tasks if state_update(task).get("candidate_kind") == "blocked"]
    split_tasks = [task for task in state_tasks if state_update(task).get("candidate_kind") == "needs_split"]

    total_expected = sum(expected_cost(task, counts) for task in tasks)
    state_expected = sum(expected_cost(task, counts) for task in state_tasks)
    safe_expected = sum(expected_cost(task, counts) for task in safe_tasks)
    blocked_expected = sum(expected_cost(task, counts) for task in blocked_tasks)
    split_expected = sum(expected_cost(task, counts) for task in split_tasks)

    blocked_cost_by_reason = Counter()
    blocked_task_count_by_reason = Counter()
    rhs_timing_expected = Counter()
    rhs_timing_task_count = Counter()
    rhs_evidence_expected = Counter()
    rhs_unknown_expected = 0
    rhs_proven_expected = 0
    rhs_same_cycle_hazard_expected = 0
    for task in state_tasks:
        expected = expected_cost(task, counts)
        su = state_update(task)
        rhs_class = su.get("rhs_timing_class", "unknown")
        rhs_timing_expected[rhs_class] += expected
        rhs_timing_task_count[rhs_class] += 1
        rhs_evidence_expected[su.get("rhs_timing_evidence", "missing")] += expected
        if rhs_class == "unknown":
            rhs_unknown_expected += expected
        else:
            rhs_proven_expected += expected
        if su.get("rhs_reads_same_cycle_target"):
            rhs_same_cycle_hazard_expected += expected
        for reason in su.get("block_reasons") or []:
            blocked_cost_by_reason[reason] += expected
            blocked_task_count_by_reason[reason] += 1

    target_tasks = defaultdict(list)
    target_expected = Counter()
    for task in state_tasks:
        expected = expected_cost(task, counts)
        for target in state_update(task).get("state_target_names") or []:
            target_tasks[target].append(task["cpp_id"])
            target_expected[target] += expected
    duplicate_targets = {
        target: {
            "task_count": len(ids),
            "cpp_ids": ids[: args.top],
            "expected_active_cost": target_expected[target],
        }
        for target, ids in target_tasks.items()
        if len(ids) > 1
    }
    duplicate_targets = dict(sorted(duplicate_targets.items(), key=lambda item: item[1]["expected_active_cost"], reverse=True))

    groups = defaultdict(lambda: {
        "task_count": 0,
        "active_count": 0,
        "expected_active_cost": 0,
        "safe_expected_active_cost": 0,
        "blocked_expected_active_cost": 0,
        "needs_split_expected_active_cost": 0,
        "cpp_id_min": None,
        "cpp_id_max": None,
    })
    for task in state_tasks:
        key = candidate_group_key(task, args.scan_range_size)
        group = groups[key]
        expected = expected_cost(task, counts)
        group["task_count"] += 1
        group["active_count"] += counts.get(task["cpp_id"], 0)
        group["expected_active_cost"] += expected
        kind = state_update(task).get("candidate_kind")
        if kind == "safe_candidate":
            group["safe_expected_active_cost"] += expected
        elif kind == "needs_split":
            group["needs_split_expected_active_cost"] += expected
        else:
            group["blocked_expected_active_cost"] += expected
        group["cpp_id_min"] = task["cpp_id"] if group["cpp_id_min"] is None else min(group["cpp_id_min"], task["cpp_id"])
        group["cpp_id_max"] = task["cpp_id"] if group["cpp_id_max"] is None else max(group["cpp_id_max"], task["cpp_id"])

    group_rows = []
    for (active_word, scan_range, clocks), group in groups.items():
        row = dict(group)
        row["active_word"] = active_word
        row["scan_range"] = scan_range
        row["clock_name"] = clocks
        group_rows.append(row)
    group_rows.sort(key=lambda row: row["expected_active_cost"], reverse=True)

    safe_group_count = sum(1 for row in group_rows if row["safe_expected_active_cost"] > 0)
    active_safe_group_count = sum(1 for row in group_rows if row["safe_expected_active_cost"] >= args.min_group_cost)
    safe_share_total = safe_expected / total_expected if total_expected else 0.0
    safe_share_state = safe_expected / state_expected if state_expected else 0.0
    threshold_met = safe_share_total >= args.min_total_share and active_safe_group_count >= 2
    if threshold_met:
        recommendation = "yes"
        recommendation_reason = "proven safe state-update cost meets threshold and spans at least two active groups"
    elif safe_expected > 0:
        recommendation = "needs one more metadata refinement"
        recommendation_reason = "some safe cost is proven, but threshold or independent group evidence is insufficient"
    else:
        recommendation = "no"
        recommendation_reason = "no state-update task is proven safe; RHS timing remains a blocking proof obligation"

    result = {
        "format": "gsim.mt-15x-state-update-proof.v1",
        "source": {
            "schedule_format": schedule.get("format"),
            "profile_cycles": profile["cycles"],
        },
        "profile": profile,
        "thresholds": {
            "min_total_share": args.min_total_share,
            "min_group_cost": args.min_group_cost,
            "scan_range_size": args.scan_range_size,
        },
        "totals": {
            "task_count": len(tasks),
            "state_update_task_count": len(state_tasks),
            "safe_candidate_task_count": len(safe_tasks),
            "blocked_state_update_task_count": len(blocked_tasks),
            "needs_split_state_update_task_count": len(split_tasks),
            "total_expected_active_cost": total_expected,
            "state_update_expected_active_cost": state_expected,
            "proven_safe_candidate_expected_active_cost": safe_expected,
            "blocked_state_update_expected_active_cost": blocked_expected,
            "needs_split_state_update_expected_active_cost": split_expected,
            "safe_share_of_total_expected_active_cost": safe_share_total,
            "safe_share_of_state_update_expected_active_cost": safe_share_state,
            "rhs_timing_proven_expected_active_cost": rhs_proven_expected,
            "rhs_timing_unknown_expected_active_cost": rhs_unknown_expected,
            "rhs_timing_unblocked_expected_active_cost": rhs_proven_expected,
            "rhs_same_cycle_hazard_expected_active_cost": rhs_same_cycle_hazard_expected,
        },
        "rhs_timing_expected_active_cost": dict(rhs_timing_expected.most_common()),
        "rhs_timing_task_count": dict(rhs_timing_task_count.most_common()),
        "rhs_timing_evidence_expected_active_cost": dict(rhs_evidence_expected.most_common()),
        "blocked_cost_by_reason": dict(blocked_cost_by_reason.most_common()),
        "blocked_task_count_by_reason": dict(blocked_task_count_by_reason.most_common()),
        "duplicate_target_counts": duplicate_targets,
        "candidate_groups": group_rows[: args.top],
        "estimated_worker_count_potential": {
            "safe_group_count": safe_group_count,
            "active_safe_group_count": active_safe_group_count,
            "threshold_met": threshold_met,
            "estimated_workers": min(max(active_safe_group_count, 1), args.max_workers) if safe_expected > 0 else 0,
            "basis": "groups are keyed by active word, scan range, and clock name; only proven safe candidate cost counts",
        },
        "top_safe_candidates": top_rows(safe_tasks, counts, args.top),
        "top_blockers": top_rows(blocked_tasks, counts, args.top),
        "recommendation": {
            "can_15x_b_implement_minimal_nba_style_parallel_state_commit": recommendation,
            "reason": recommendation_reason,
            "next_bottleneck": "prove RHS timing as precomputed or old_state_only before adding runtime parallel commit",
        },
    }
    return result


def markdown_table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    lines.extend("| " + " | ".join(str(value) for value in row) + " |" for row in rows)
    return "\n".join(lines)


def write_markdown(result, path):
    totals = result["totals"]
    recommendation = result["recommendation"]
    potential = result["estimated_worker_count_potential"]
    lines = [
        "# GSim-MT 15X State-Update Proof Report",
        "",
        "## Decision",
        "",
        markdown_table(
            ["question", "answer"],
            [
                ["Can 15X-B implement minimal NBA-style parallel state commit?", recommendation["can_15x_b_implement_minimal_nba_style_parallel_state_commit"]],
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
                ["rhs_timing_proven_expected_active_cost", totals["rhs_timing_proven_expected_active_cost"]],
                ["rhs_timing_unknown_expected_active_cost", totals["rhs_timing_unknown_expected_active_cost"]],
                ["proven_safe_candidate_expected_active_cost", totals["proven_safe_candidate_expected_active_cost"]],
                ["blocked_state_update_expected_active_cost", totals["blocked_state_update_expected_active_cost"]],
                ["needs_split_state_update_expected_active_cost", totals["needs_split_state_update_expected_active_cost"]],
                ["safe_share_of_total_expected_active_cost", f"{totals['safe_share_of_total_expected_active_cost']:.6f}"],
                ["safe_share_of_state_update_expected_active_cost", f"{totals['safe_share_of_state_update_expected_active_cost']:.6f}"],
            ],
        ),
        "",
        "## RHS Timing",
        "",
        markdown_table(
            ["class", "expected_active_cost", "task_count"],
            [
                [name, cost, result["rhs_timing_task_count"].get(name, 0)]
                for name, cost in result["rhs_timing_expected_active_cost"].items()
            ] or [["<none>", 0, 0]],
        ),
        "",
        "## RHS Timing Evidence",
        "",
        markdown_table(
            ["evidence", "expected_active_cost"],
            [
                [name, cost]
                for name, cost in result["rhs_timing_evidence_expected_active_cost"].items()
            ] or [["<none>", 0]],
        ),
        "",
        "## Worker Potential",
        "",
        markdown_table(
            ["metric", "value"],
            [
                ["safe_group_count", potential["safe_group_count"]],
                ["active_safe_group_count", potential["active_safe_group_count"]],
                ["estimated_workers", potential["estimated_workers"]],
                ["threshold_met", potential["threshold_met"]],
                ["basis", potential["basis"]],
            ],
        ),
        "",
        "## Blocked Cost By Reason",
        "",
        markdown_table(
            ["reason", "expected_active_cost", "task_count"],
            [
                [reason, cost, result["blocked_task_count_by_reason"].get(reason, 0)]
                for reason, cost in result["blocked_cost_by_reason"].items()
            ] or [["<none>", 0, 0]],
        ),
        "",
        "## Duplicate Targets",
        "",
        markdown_table(
            ["target", "task_count", "expected_active_cost", "cpp_ids"],
            [
                [target, item["task_count"], item["expected_active_cost"], ",".join(str(v) for v in item["cpp_ids"])]
                for target, item in list(result["duplicate_target_counts"].items())[:10]
            ] or [["<none>", 0, 0, ""]],
        ),
        "",
        "## Candidate Groups",
        "",
        markdown_table(
            ["active_word", "scan_range", "clock_name", "task_count", "expected_active_cost", "safe_cost", "blocked_cost"],
            [
                [
                    row["active_word"],
                    row["scan_range"],
                    row["clock_name"],
                    row["task_count"],
                    row["expected_active_cost"],
                    row["safe_expected_active_cost"],
                    row["blocked_expected_active_cost"],
                ]
                for row in result["candidate_groups"]
            ] or [["<none>", "", "", 0, 0, 0, 0]],
        ),
        "",
        "## Top Safe Candidates",
        "",
        markdown_table(
            ["cpp_id", "expected_active_cost", "target", "active_word", "rhs_timing_class", "rhs_timing_evidence"],
            [
                [
                    row["cpp_id"],
                    row["expected_active_cost"],
                    ",".join(row["state_target_names"]),
                    row["active_word"],
                    row["rhs_timing_class"],
                    row["rhs_timing_evidence"],
                ]
                for row in result["top_safe_candidates"]
            ] or [["<none>", 0, "", "", "", ""]],
        ),
        "",
        "## Top Blockers",
        "",
        markdown_table(
            ["cpp_id", "expected_active_cost", "target", "active_word", "rhs_timing_class", "block_reasons"],
            [
                [
                    row["cpp_id"],
                    row["expected_active_cost"],
                    ",".join(row["state_target_names"]),
                    row["active_word"],
                    row["rhs_timing_class"],
                    ",".join(row["block_reasons"]),
                ]
                for row in result["top_blockers"]
            ] or [["<none>", 0, "", "", "", ""]],
        ),
        "",
    ]
    path.write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description="Report conservative 15X state-update proof metadata against MT task profile data.")
    parser.add_argument("--schedule-json", required=True, help="Path to *_mt_schedule.json generated with 15X metadata")
    parser.add_argument("--profile-log", required=True, help="stderr/stdout log containing GSIM_MT_PROFILE_TASKS=1 mt-profile task_cpp_ids")
    parser.add_argument("--out-json", help="Optional output JSON path")
    parser.add_argument("--out-md", help="Optional output Markdown path")
    parser.add_argument("--top", type=int, default=20, help="Maximum rows in top/group sections")
    parser.add_argument("--min-total-share", type=float, default=0.05, help="Safe cost share of total expected active cost needed for 15X-B")
    parser.add_argument("--min-group-cost", type=int, default=1, help="Minimum safe expected cost for an active worker group")
    parser.add_argument("--scan-range-size", type=int, default=64, help="Scan-index grouping range size")
    parser.add_argument("--max-workers", type=int, default=8, help="Cap for estimated worker count potential")
    args = parser.parse_args()

    expect(args.top > 0, "--top must be positive")
    expect(args.scan_range_size > 0, "--scan-range-size must be positive")
    schedule_path = Path(args.schedule_json)
    profile_path = Path(args.profile_log)
    expect(schedule_path.is_file(), f"{schedule_path} does not exist")
    expect(profile_path.is_file(), f"{profile_path} does not exist")

    schedule = json.loads(schedule_path.read_text())
    result = summarize(schedule, profile_path.read_text(errors="replace"), args)

    if args.out_json:
        Path(args.out_json).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    if args.out_md:
        write_markdown(result, Path(args.out_md))
    if not args.out_json and not args.out_md:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        totals = result["totals"]
        decision = result["recommendation"]["can_15x_b_implement_minimal_nba_style_parallel_state_commit"]
        print(
            "state_update_expected_active_cost="
            f"{totals['state_update_expected_active_cost']} "
            "proven_safe_candidate_expected_active_cost="
            f"{totals['proven_safe_candidate_expected_active_cost']} "
            f"decision={decision}"
        )


if __name__ == "__main__":
    main()
