#!/usr/bin/env python3
import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


RESET_REASONS = {"reset", "async_reset", "activate_all_path"}
MEMORY_DYNAMIC_REASONS = {
    "memory_write",
    "memory_read_unsupported",
    "array_or_dynamic_index",
}
SPECIAL_UNKNOWN_REASONS = {
    "external",
    "special",
    "unknown_node",
    "unknown_op",
    "super_type_SUPER_EXTMOD",
    "super_type_SUPER_ASYNC_RESET",
    "super_type_SUPER_UINT_RESET",
    "super_type_SUPER_UPDATE_REG",
}


def fail(message):
    print(f"mt-14x-state-update-and-repcut-report failed: {message}", file=sys.stderr)
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


def has_any_reason(task, reasons):
    return bool(set(task.get("serial_reasons") or []).intersection(reasons))


def is_state_update(task):
    return "state_update" in (task.get("serial_reasons") or [])


def is_syntactic_simple_state_candidate(task):
    reasons = task.get("serial_reasons") or []
    boundary = task.get("boundary") or {}
    node_kinds = task.get("node_kinds") or {}
    reg_like_count = (
        node_kinds.get("NODE_REG_DST", 0)
        + node_kinds.get("NODE_REG_RESET", 0)
        + node_kinds.get("NODE_REG_SRC", 0)
    )
    return (
        reasons == ["state_update"]
        and reg_like_count == 1
        and not boundary.get("has_memory_write", False)
        and not boundary.get("has_reset", False)
        and not boundary.get("has_external", False)
        and not boundary.get("has_special", False)
    )


def summarize_bucket(name, tasks, counts, basis):
    return {
        "name": name,
        "task_count": len(tasks),
        "active_count": sum(counts.get(task["cpp_id"], 0) for task in tasks),
        "expected_active_cost": sum(expected_cost(task, counts) for task in tasks),
        "basis": basis,
    }


def top_rows(tasks, counts, limit):
    rows = []
    for task in tasks:
        rows.append({
            "cpp_id": task["cpp_id"],
            "active_count": counts.get(task["cpp_id"], 0),
            "static_cost": static_cost(task),
            "expected_active_cost": expected_cost(task, counts),
            "serial_reasons": task.get("serial_reasons", []),
            "node_kinds": task.get("node_kinds", {}),
            "active_word": task.get("active_word"),
            "active_fanout": task.get("active_fanout", []),
            "active_fanout_count": len(task.get("active_fanout", [])),
            "succ_cpp_ids": task.get("succ_cpp_ids", []),
        })
    rows.sort(key=lambda row: row["expected_active_cost"], reverse=True)
    return rows[:limit]


def histogram_by_count(tasks, key_fn):
    counter = Counter()
    for task in tasks:
        counter[key_fn(task)] += 1
    return dict(counter.most_common())


def cost_by_key(tasks, counts, key_fn):
    counter = Counter()
    for task in tasks:
        counter[str(key_fn(task))] += expected_cost(task, counts)
    return dict(counter.most_common())


def plan_repcut(candidates, tasks_by_id, total_expected, pure_expected, args):
    planned = []
    for candidate in candidates:
        copy_cost = int(candidate.get("copy_cost", 0))
        fanout = int(candidate.get("fanout", 0))
        expected = int(candidate.get("expected_active_cost", 0))
        ratio = expected / max(copy_cost, 1)
        row = dict(candidate)
        row["released_per_copy_cost"] = ratio
        row["accepted"] = False
        row["reject_reason"] = ""
        row["budget_rank"] = 0
        task = tasks_by_id.get(candidate.get("cpp_id"))
        row["active_word"] = task.get("active_word") if task else None
        planned.append(row)

    planned.sort(key=lambda row: (row["released_per_copy_cost"], row["expected_active_cost"]), reverse=True)

    total_copy_used = 0
    for rank, row in enumerate(planned, start=1):
        row["budget_rank"] = rank
        copy_cost = int(row.get("copy_cost", 0))
        fanout = int(row.get("fanout", 0))
        if copy_cost > args.copy_budget:
            row["reject_reason"] = f"copy_cost>{args.copy_budget}"
        elif fanout > args.fanout_budget:
            row["reject_reason"] = f"fanout>{args.fanout_budget}"
        elif total_copy_used + copy_cost > args.total_copy_budget:
            row["reject_reason"] = f"total_copy_budget>{args.total_copy_budget}"
        else:
            row["accepted"] = True
            row["reject_reason"] = "accepted"
            total_copy_used += copy_cost

    accepted = [row for row in planned if row["accepted"]]
    released = sum(int(row.get("expected_active_cost", 0)) for row in accepted)
    by_word = defaultdict(lambda: {"candidate_count": 0, "expected_active_cost": 0})
    for row in accepted:
        key = str(row.get("active_word"))
        by_word[key]["candidate_count"] += 1
        by_word[key]["expected_active_cost"] += int(row.get("expected_active_cost", 0))
    two_worker_proxy = sum(
        item["expected_active_cost"]
        for item in by_word.values()
        if item["candidate_count"] >= 2
    )
    released_share_total = released / total_expected if total_expected else 0.0
    released_share_pure = released / pure_expected if pure_expected else 0.0
    decision = "defer_runtime_on"
    if released_share_total >= args.min_total_share and two_worker_proxy > 0:
        decision = "planner_promising_but_report_only"
    return {
        "budgets": {
            "copy_budget": args.copy_budget,
            "fanout_budget": args.fanout_budget,
            "total_copy_budget": args.total_copy_budget,
            "min_total_share": args.min_total_share,
        },
        "candidate_count": len(planned),
        "accepted_count": len(accepted),
        "total_copy_used": total_copy_used,
        "predicted_released_active_cost": released,
        "predicted_released_share_of_total": released_share_total,
        "predicted_released_share_of_pure": released_share_pure,
        "same_active_word_two_worker_available_cost_proxy": two_worker_proxy,
        "accepted_active_word_distribution": dict(sorted(by_word.items(), key=lambda item: item[1]["expected_active_cost"], reverse=True)),
        "decision": decision,
        "candidates": planned,
    }


def summarize(schedule, profile_text, activity, args):
    tasks = schedule.get("tasks")
    expect(isinstance(tasks, list) and tasks, "schedule JSON has no tasks")
    counts = parse_task_counts(profile_text)
    profile = parse_profile_summary(profile_text)
    tasks_by_id = {task["cpp_id"]: task for task in tasks}

    total_expected = sum(expected_cost(task, counts) for task in tasks)
    pure_expected = sum(expected_cost(task, counts) for task in tasks if task.get("task_kind") == "pure_compute")
    serial_expected = total_expected - pure_expected
    state_tasks = [task for task in tasks if is_state_update(task)]
    state_only = [task for task in state_tasks if task.get("serial_reasons") == ["state_update"]]
    syntactic = [task for task in state_tasks if is_syntactic_simple_state_candidate(task)]
    syntactic_ids = {task["cpp_id"] for task in syntactic}
    composite_state_only = [task for task in state_only if task["cpp_id"] not in syntactic_ids]
    reset_blocked = [task for task in state_tasks if has_any_reason(task, RESET_REASONS)]
    memory_blocked = [task for task in state_tasks if has_any_reason(task, MEMORY_DYNAMIC_REASONS)]
    special_blocked = [task for task in state_tasks if has_any_reason(task, SPECIAL_UNKNOWN_REASONS)]
    active_visibility_blocked = [task for task in syntactic if task.get("active_fanout")]

    categories = [
        summarize_bucket(
            "state_only_upper_bound_proxy",
            state_only,
            counts,
            "serial_reasons exactly ['state_update']; includes simple and composite register-like updates",
        ),
        summarize_bucket(
            "simple_single_register_like_commit_candidates_proxy",
            syntactic,
            counts,
            "state-only tasks with exactly one NODE_REG_SRC/NODE_REG_DST/NODE_REG_RESET; v1 schedule still does not prove target identity or RHS timing",
        ),
        summarize_bucket(
            "composite_state_only_multiple_register_like_blocked_proxy",
            composite_state_only,
            counts,
            "state-only tasks with multiple register-like nodes; require splitting or commit-group proof before parallelization",
        ),
        summarize_bucket(
            "reset_or_async_reset_blocked_updates",
            reset_blocked,
            counts,
            "state_update tasks with reset, async_reset, or activate_all_path reasons",
        ),
        summarize_bucket(
            "memory_write_or_dynamic_array_blocked_updates",
            memory_blocked,
            counts,
            "state_update tasks with memory write/read unsupported or array/dynamic-index reasons",
        ),
        summarize_bucket(
            "external_special_trace_unknown_blocked_updates",
            special_blocked,
            counts,
            "state_update tasks with external, special, unknown, or non-SUPER_VALID reasons; trace has no separate v1 schedule flag",
        ),
        {
            "name": "same_target_or_multiple_writer_blocked_updates_detected",
            "task_count": 0,
            "active_count": 0,
            "expected_active_cost": 0,
            "basis": "v1 schedule has no register target identity, so concrete duplicate-target detection is unavailable",
        },
        summarize_bucket(
            "same_target_or_multiple_writer_proof_missing_candidates",
            state_only,
            counts,
            "all state-only candidates still need target identity and duplicate-writer proof before they are safe",
        ),
        summarize_bucket(
            "active_visibility_blocked_updates",
            active_visibility_blocked,
            counts,
            "syntactic candidates with non-empty active_fanout; future work must record state-commit activation in ActivationDelta and merge deterministically",
        ),
    ]

    state_expected = sum(expected_cost(task, counts) for task in state_tasks)
    state_only_expected = sum(expected_cost(task, counts) for task in state_only)
    syntactic_expected = sum(expected_cost(task, counts) for task in syntactic)
    safe_groups = []
    safe_cost = 0
    safe_worker_potential = {
        "safe_group_count": 0,
        "max_workers_from_safe_groups": 1,
        "reason": "no state-update task is proven safe because v1 schedule lacks target identity, duplicate-writer, and RHS timing metadata",
    }

    syntactic_by_word = defaultdict(lambda: {"task_count": 0, "expected_active_cost": 0})
    for task in syntactic:
        word = str(task.get("active_word"))
        syntactic_by_word[word]["task_count"] += 1
        syntactic_by_word[word]["expected_active_cost"] += expected_cost(task, counts)

    activity_totals = {
        "total_expected_active_cost": activity.get("total_expected_active_cost", total_expected),
        "pure_expected_active_cost": activity.get("pure_expected_active_cost", pure_expected),
        "serial_expected_active_cost": activity.get("serial_expected_active_cost", serial_expected),
        "state_update_blocker_expected_active_cost": (activity.get("blocker_expected_active_cost") or {}).get("state_update", state_expected),
    }
    repcut = plan_repcut(
        activity.get("repcut_lite_candidates") or [],
        tasks_by_id,
        activity_totals["total_expected_active_cost"],
        activity_totals["pure_expected_active_cost"],
        args,
    )

    return {
        "format": "gsim.mt-14x-state-update-and-repcut-report.v1",
        "inputs": {
            "schedule_json": str(args.schedule_json),
            "profile_log": str(args.profile_log),
            "activity_json": str(args.activity_json),
        },
        "profile": profile,
        "source_findings": {
            "state_update_classification": "collectMtBoundaryInfo marks state update for SUPER_UPDATE_REG, NODE_REG_DST, NODE_REG_RESET, or NODE_REG_SRC with a valid regNext",
            "serial_reason_classification": "classifyMtTask adds serial reasons for state_update, memory read/write, reset/async reset, activate_all_path, external, special, unknown node/op, and array/dynamic index",
            "rhs_timing_evidence": "v1 schedule does not expose whether RHS is already determined before a future commit group",
            "target_identity_evidence": "v1 schedule does not expose register target object identity",
            "activation_effect_evidence": "activateNext combines state write and activation; helper paths can route activation through ActiveBuffer/ActivationDelta when a sink object is passed",
        },
        "activity_totals": activity_totals,
        "computed_totals_from_schedule_profile": {
            "task_count": len(tasks),
            "active_task_count": sum(1 for task in tasks if counts.get(task["cpp_id"], 0) > 0),
            "total_expected_active_cost": total_expected,
            "pure_expected_active_cost": pure_expected,
            "serial_expected_active_cost": serial_expected,
            "state_update_task_count": len(state_tasks),
            "state_update_expected_active_cost": state_expected,
            "state_only_upper_bound_expected_active_cost": state_only_expected,
            "syntactic_simple_single_register_like_candidate_expected_active_cost": syntactic_expected,
            "estimated_safe_parallelizable_state_update_cost": safe_cost,
        },
        "state_update_categories": categories,
        "top_state_update_tasks": top_rows(state_tasks, counts, args.top),
        "state_update_reason_combo_counts": histogram_by_count(state_tasks, lambda task: ",".join(task.get("serial_reasons", []))),
        "syntactic_candidate_activation_fanout_distribution": histogram_by_count(syntactic, lambda task: len(task.get("active_fanout") or [])),
        "syntactic_candidate_active_word_expected_active_cost": dict(sorted(syntactic_by_word.items(), key=lambda item: item[1]["expected_active_cost"], reverse=True)[: args.top]),
        "safe_groups": safe_groups,
        "safe_group_activation_fanout_distribution": {},
        "safe_group_active_word_write_distribution": {},
        "estimated_worker_count_potential_from_safe_groups": safe_worker_potential,
        "unresolved_proof_obligations": [
            "add schedule metadata for the single register-like target written by each state-update task",
            "prove no duplicate target appears in a future parallel commit group",
            "prove each RHS value is determined before commit or computed only from old state and inputs",
            "separate or preserve activateNext state write and activation side effects using ActivationDelta",
            "preserve deterministic activation merge order and original active visibility",
            "preserve async reset pre-reset and post-reset observation points",
            "keep memory writes, dynamic arrays, external/DPI, printf/assert/stop, trace, and unknown operations serial",
        ],
        "feasibility_decision": {
            "state_update_parallelism": "not_ready_for_runtime_implementation",
            "reason": "large syntactic state-update cost exists, but current evidence cannot prove the correctness obligations for any safe group",
            "static_expected_cost_is_runtime_proxy": True,
            "estimated_safe_parallelizable_state_update_cost": safe_cost,
        },
        "repcut_lite_planner": repcut,
    }


def markdown_table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    for row in rows:
        lines.append("| " + " | ".join(str(value) for value in row) + " |")
    return "\n".join(lines)


def fmt_pct(value):
    return f"{value * 100:.3f}%"


def write_markdown(result, path):
    totals = result["computed_totals_from_schedule_profile"]
    activity = result["activity_totals"]
    repcut = result["repcut_lite_planner"]
    lines = [
        "# GSim-MT 14X State-Update Feasibility And RepCut-Lite Plan",
        "",
        "This report is based on 13X schedule/profile data. Expected active cost is a static proxy computed from task activation counts and static node/candidate cost; it is not measured runtime.",
        "",
        "## Inputs",
        "",
        markdown_table(
            ["input", "path"],
            [[key, value] for key, value in result["inputs"].items()],
        ),
        "",
        "## Source Findings",
        "",
        markdown_table(
            ["question", "finding"],
            [
                ["state_update classification", result["source_findings"]["state_update_classification"]],
                ["serial reason classification", result["source_findings"]["serial_reason_classification"]],
                ["RHS timing", result["source_findings"]["rhs_timing_evidence"]],
                ["target identity", result["source_findings"]["target_identity_evidence"]],
                ["activation effects", result["source_findings"]["activation_effect_evidence"]],
            ],
        ),
        "",
        "## 14X-A State-Update Feasibility",
        "",
        markdown_table(
            ["metric", "value"],
            [
                ["task_count", totals["task_count"]],
                ["active_task_count", totals["active_task_count"]],
                ["total_expected_active_cost", totals["total_expected_active_cost"]],
                ["pure_expected_active_cost", totals["pure_expected_active_cost"]],
                ["serial_expected_active_cost", totals["serial_expected_active_cost"]],
                ["state_update_task_count", totals["state_update_task_count"]],
                ["state_update_expected_active_cost", totals["state_update_expected_active_cost"]],
                ["13X_state_update_blocker_expected_active_cost", activity["state_update_blocker_expected_active_cost"]],
                ["state_only_upper_bound_expected_active_cost", totals["state_only_upper_bound_expected_active_cost"]],
                ["syntactic_simple_single_register_like_candidate_expected_active_cost", totals["syntactic_simple_single_register_like_candidate_expected_active_cost"]],
                ["estimated_safe_parallelizable_state_update_cost", totals["estimated_safe_parallelizable_state_update_cost"]],
            ],
        ),
        "",
        "### State-Update Categories",
        "",
        markdown_table(
            ["category", "task_count", "active_count", "expected_active_cost", "basis"],
            [
                [
                    row["name"],
                    row["task_count"],
                    row["active_count"],
                    row["expected_active_cost"],
                    row["basis"],
                ]
                for row in result["state_update_categories"]
            ],
        ),
        "",
        "### Top State-Update Tasks",
        "",
        markdown_table(
            ["cpp_id", "active_count", "static_cost", "expected_active_cost", "active_word", "active_fanout_count", "active_fanout_sample", "serial_reasons"],
            [
                [
                    row["cpp_id"],
                    row["active_count"],
                    row["static_cost"],
                    row["expected_active_cost"],
                    row["active_word"],
                    row["active_fanout_count"],
                    ",".join(str(value) for value in row["active_fanout"][:8]),
                    ",".join(row["serial_reasons"]),
                ]
                for row in result["top_state_update_tasks"]
            ],
        ),
        "",
        "### Candidate Activation Proxy",
        "",
        "No safe state-update group is proven in 14X. The following distributions describe only syntactic candidates, not runtime-safe work.",
        "",
        markdown_table(
            ["fanout", "syntactic_candidate_task_count"],
            [[key, value] for key, value in list(result["syntactic_candidate_activation_fanout_distribution"].items())[:30]],
        ),
        "",
        markdown_table(
            ["active_word", "task_count", "expected_active_cost"],
            [
                [word, value["task_count"], value["expected_active_cost"]]
                for word, value in result["syntactic_candidate_active_word_expected_active_cost"].items()
            ],
        ),
        "",
        "### Feasibility Decision",
        "",
        markdown_table(
            ["field", "value"],
            [
                ["state_update_parallelism", result["feasibility_decision"]["state_update_parallelism"]],
                ["reason", result["feasibility_decision"]["reason"]],
                ["static_expected_cost_is_runtime_proxy", result["feasibility_decision"]["static_expected_cost_is_runtime_proxy"]],
                ["safe_group_count", result["estimated_worker_count_potential_from_safe_groups"]["safe_group_count"]],
                ["max_workers_from_safe_groups", result["estimated_worker_count_potential_from_safe_groups"]["max_workers_from_safe_groups"]],
            ],
        ),
        "",
        "### Unresolved Proof Obligations",
        "",
        "\n".join(f"- {item}" for item in result["unresolved_proof_obligations"]),
        "",
        "## 14X-B Bounded RepCut-Lite Planner",
        "",
        markdown_table(
            ["budget", "value"],
            [[key, value] for key, value in repcut["budgets"].items()],
        ),
        "",
        markdown_table(
            ["metric", "value"],
            [
                ["candidate_count", repcut["candidate_count"]],
                ["accepted_count", repcut["accepted_count"]],
                ["total_copy_used", repcut["total_copy_used"]],
                ["predicted_released_active_cost", repcut["predicted_released_active_cost"]],
                ["predicted_released_share_of_total", fmt_pct(repcut["predicted_released_share_of_total"])],
                ["predicted_released_share_of_pure", fmt_pct(repcut["predicted_released_share_of_pure"])],
                ["same_active_word_two_worker_available_cost_proxy", repcut["same_active_word_two_worker_available_cost_proxy"]],
                ["decision", repcut["decision"]],
            ],
        ),
        "",
        "### Candidate Plan",
        "",
        markdown_table(
            ["rank", "cpp_id", "expected_active_cost", "copy_cost", "fanout", "released_per_copy", "active_word", "accepted", "reason"],
            [
                [
                    row["budget_rank"],
                    row["cpp_id"],
                    row["expected_active_cost"],
                    row["copy_cost"],
                    row["fanout"],
                    f"{row['released_per_copy_cost']:.3f}",
                    row.get("active_word"),
                    row["accepted"],
                    row["reject_reason"],
                ]
                for row in repcut["candidates"]
            ],
        ),
        "",
        "## Non-Claims",
        "",
        "- This report does not implement a runtime state-update parallel path.",
        "- This report does not enable XiangShan `--mt-repcut-lite=on`.",
        "- No XiangShan speedup is claimed from static expected-cost proxies.",
        "",
    ]
    path.write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--schedule-json", type=Path, required=True)
    parser.add_argument("--profile-log", type=Path, required=True)
    parser.add_argument("--activity-json", type=Path, required=True)
    parser.add_argument("--out-json", type=Path, required=True)
    parser.add_argument("--out-md", type=Path, required=True)
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--copy-budget", type=int, default=4096)
    parser.add_argument("--fanout-budget", type=int, default=4)
    parser.add_argument("--total-copy-budget", type=int, default=8192)
    parser.add_argument("--min-total-share", type=float, default=0.05)
    args = parser.parse_args()

    for input_path in (args.schedule_json, args.profile_log, args.activity_json):
        expect(input_path.is_file(), f"{input_path} does not exist")

    schedule = json.loads(args.schedule_json.read_text())
    activity = json.loads(args.activity_json.read_text())
    profile_text = args.profile_log.read_text()
    result = summarize(schedule, profile_text, activity, args)

    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_md.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    write_markdown(result, args.out_md)
    print(f"wrote {args.out_json}")
    print(f"wrote {args.out_md}")
    print(
        "state_update_expected_active_cost="
        f"{result['computed_totals_from_schedule_profile']['state_update_expected_active_cost']} "
        "safe_parallelizable_state_update_cost="
        f"{result['computed_totals_from_schedule_profile']['estimated_safe_parallelizable_state_update_cost']} "
        "repcut_decision="
        f"{result['repcut_lite_planner']['decision']}"
    )


if __name__ == "__main__":
    main()
