#!/usr/bin/env python3
import argparse
import json
import math
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path


TARGETS_DEFAULT = (2, 4, 8, 16)

MEMORY_REASONS = {"memory_write", "memory_read_unsupported", "array_or_dynamic_index", "memory_or_dynamic_array"}
RESET_REASONS = {"reset", "async_reset", "activate_all_path", "reset_behavior", "async_reset_behavior"}
EXTERNAL_REASONS = {"external", "super_type_SUPER_EXTMOD"}
SPECIAL_REASONS = {
    "special",
    "unknown_node",
    "unknown_op",
    "super_type_SUPER_ASYNC_RESET",
    "super_type_SUPER_UINT_RESET",
    "super_type_SUPER_UPDATE_REG",
    "external_or_special_or_unknown",
}
STATE_UPDATE_REASONS = {"state_update", "target_identity_ambiguous", "multiple_state_targets"}


def fail(message):
    print(f"mt-22x-macro-fiber-repcut-planner failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def read_json(path):
    try:
        return json.loads(path.read_text())
    except Exception as exc:
        fail(f"failed to read JSON {path}: {exc}")


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
    coarse_match = re.search(r"\[mt-profile\] coarse_dispatch .*$", text, re.M)
    assignment_match = re.search(r"\[mt-profile\] coarse_assignment .*$", text, re.M)
    return {
        "cycles": cycles,
        "main": main,
        "coarse_dispatch": parse_key_values(coarse_match.group(0)) if coarse_match else {},
        "coarse_assignment": parse_key_values(assignment_match.group(0)) if assignment_match else {},
    }


def git_commit(repo_root):
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=repo_root,
            text=True,
            capture_output=True,
            check=True,
        )
        return result.stdout.strip()
    except Exception:
        return "unknown"


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


def has_any(task, reasons):
    return bool(set(task.get("serial_reasons") or []).intersection(reasons))


def blocker_reason(task):
    boundary = task.get("boundary") or {}
    state_update = task.get("state_update") or {}
    group = task.get("state_update_group") or {}
    reasons = set(task.get("serial_reasons") or [])
    if task.get("super_type") != "SUPER_VALID":
        if task.get("super_type") == "SUPER_EXTMOD":
            return "external"
        return "serial_side_effect"
    if boundary.get("has_memory_write") or has_any(task, MEMORY_REASONS):
        return "memory"
    if boundary.get("has_reset") or has_any(task, RESET_REASONS):
        return "reset"
    if boundary.get("has_external") or has_any(task, EXTERNAL_REASONS):
        return "external"
    if boundary.get("has_special") or has_any(task, SPECIAL_REASONS):
        return "serial_side_effect"
    if boundary.get("has_state_update") or state_update.get("has_state_update") or has_any(task, STATE_UPDATE_REASONS):
        if group.get("runtime_safe_candidate"):
            return None
        if group.get("local_safe_candidate"):
            return "state_update_proof"
        return "state_update_proof"
    if task.get("task_kind") != "pure_compute":
        return "serial_side_effect"
    return None


def safety_kind(task):
    boundary = task.get("boundary") or {}
    state_update = task.get("state_update") or {}
    group = task.get("state_update_group") or {}
    if boundary.get("has_state_update") or state_update.get("has_state_update") or "state_update" in (task.get("serial_reasons") or []):
        if group.get("runtime_safe_candidate"):
            return "runtime-safe-state-rooted"
        if group.get("local_safe_candidate"):
            return "local-safe-state-rooted"
        return "optimistic-only"
    if task.get("task_kind") == "pure_compute":
        return "pure-only"
    return "optimistic-only"


def is_sink_like(task, succs_by_id):
    cpp_id = task["cpp_id"]
    node_kinds = task.get("node_kinds") or {}
    boundary = task.get("boundary") or {}
    state_update = task.get("state_update") or {}
    if node_kinds.get("NODE_OUT", 0) > 0:
        return True
    if boundary.get("has_state_update") or state_update.get("has_state_update") or "state_update" in (task.get("serial_reasons") or []):
        return True
    return len(succs_by_id.get(cpp_id, [])) == 0


def unique_sorted(values):
    return sorted(set(values))


def build_graph(tasks):
    tasks_by_id = {task["cpp_id"]: task for task in tasks}
    preds_by_id = {task["cpp_id"]: unique_sorted(task.get("pred_cpp_ids") or []) for task in tasks}
    succs_by_id = {task["cpp_id"]: unique_sorted(task.get("succ_cpp_ids") or []) for task in tasks}
    for task in tasks:
        cpp_id = task["cpp_id"]
        for pred_id in task.get("pred_cpp_ids") or []:
            if pred_id in tasks_by_id:
                succs_by_id.setdefault(pred_id, [])
                if cpp_id not in succs_by_id[pred_id]:
                    succs_by_id[pred_id].append(cpp_id)
        for succ_id in task.get("succ_cpp_ids") or []:
            if succ_id in tasks_by_id:
                preds_by_id.setdefault(succ_id, [])
                if cpp_id not in preds_by_id[succ_id]:
                    preds_by_id[succ_id].append(cpp_id)
    return tasks_by_id, preds_by_id, succs_by_id


def build_fiber(root, tasks_by_id, preds_by_id, counts, max_depth):
    body = set()
    blocked = []
    stack = [(root["cpp_id"], 0)]
    while stack:
        cpp_id, depth = stack.pop()
        if cpp_id in body:
            continue
        task = tasks_by_id.get(cpp_id)
        if task is None:
            continue
        reason = blocker_reason(task)
        if reason:
            blocked.append({
                "cpp_id": cpp_id,
                "super_id": task.get("super_id"),
                "reason": reason,
                "static_cost": static_cost(task),
                "expected_active_cost": expected_cost(task, counts),
                "serial_reasons": task.get("serial_reasons") or [],
            })
            if cpp_id == root["cpp_id"]:
                body.add(cpp_id)
            continue
        body.add(cpp_id)
        if depth >= max_depth:
            continue
        for pred_id in reversed(preds_by_id.get(cpp_id, [])):
            stack.append((pred_id, depth + 1))
    return body, blocked


def build_candidates(tasks, counts, profile, max_depth):
    tasks_by_id, preds_by_id, succs_by_id = build_graph(tasks)
    candidates = []
    for task in tasks:
        if not is_sink_like(task, succs_by_id):
            continue
        body, blocked = build_fiber(task, tasks_by_id, preds_by_id, counts, max_depth)
        body_tasks = [tasks_by_id[cpp_id] for cpp_id in sorted(body)]
        if not body_tasks:
            continue
        expected = sum(expected_cost(item, counts) for item in body_tasks)
        static = sum(static_cost(item) for item in body_tasks)
        candidate = {
            "root_cpp_id": task["cpp_id"],
            "root_super_id": task.get("super_id"),
            "safety_kind": safety_kind(task),
            "cpp_ids": [item["cpp_id"] for item in body_tasks],
            "super_ids": [item.get("super_id") for item in body_tasks],
            "task_count": len(body_tasks),
            "static_cost": static,
            "active_count": sum(counts.get(item["cpp_id"], 0) for item in body_tasks),
            "active_frequency": (sum(counts.get(item["cpp_id"], 0) for item in body_tasks) / max(profile["cycles"], 1)),
            "expected_active_cost": expected,
            "blocked_units": blocked,
        }
        candidates.append(candidate)
    candidates.sort(key=lambda row: (row["expected_active_cost"], row["static_cost"], row["root_cpp_id"]), reverse=True)
    return candidates, tasks_by_id, preds_by_id, succs_by_id


def clone_candidate(candidate):
    return {
        "root_cpp_id": candidate["root_cpp_id"],
        "cpp_ids": list(candidate["cpp_ids"]),
        "static_cost": candidate["static_cost"],
        "expected_active_cost": candidate["expected_active_cost"],
        "partition": None,
    }


def greedy_assign(items, target, cost_key):
    partitions = [{"index": idx, "cost": 0, "static_cost": 0, "expected_cost": 0, "items": []} for idx in range(target)]
    rows = sorted(items, key=lambda item: (item[cost_key], item.get("expected_active_cost", 0), item["root_cpp_id"]), reverse=True)
    for item in rows:
        partition = min(partitions, key=lambda row: (row["cost"], row["index"]))
        partition["items"].append(item)
        partition["cost"] += item[cost_key]
        partition["static_cost"] += item.get("static_cost", 0)
        partition["expected_cost"] += item.get("expected_active_cost", 0)
        item["partition"] = partition["index"]
    return partitions


def assignment_summary(items, target, cost_key):
    assigned = [clone_candidate(item) for item in items]
    partitions = greedy_assign(assigned, target, cost_key)
    expected_costs = [row["expected_cost"] for row in partitions]
    static_costs = [row["static_cost"] for row in partitions]
    expected_total = sum(expected_costs)
    static_total = sum(static_costs)
    return {
        "cost_key": cost_key,
        "expected_costs": expected_costs,
        "static_costs": static_costs,
        "expected_max": max(expected_costs) if expected_costs else 0,
        "expected_avg": expected_total / target if target else 0,
        "expected_imbalance_ratio": (max(expected_costs) / (expected_total / target)) if expected_total and target else 0,
        "static_max": max(static_costs) if static_costs else 0,
        "static_avg": static_total / target if target else 0,
        "static_imbalance_ratio": (max(static_costs) / (static_total / target)) if static_total and target else 0,
        "partition_roots": [[item["root_cpp_id"] for item in row["items"]] for row in partitions],
    }


def duplicate_block_reason(task, expected, args):
    reason = blocker_reason(task)
    if reason:
        return reason
    if expected <= 0:
        return "inactive"
    if expected > args.max_duplicate_expected_cost:
        return "replication_budget"
    return None


def plan_replication(candidates, tasks_by_id, preds_by_id, succs_by_id, counts, target, args):
    items = [clone_candidate(candidate) for candidate in candidates if candidate["expected_active_cost"] > 0]
    active_assignment = assignment_summary(items, target, "expected_active_cost")
    static_assignment = assignment_summary(items, target, "static_cost")
    root_partition = {}
    for partition_idx, roots in enumerate(active_assignment["partition_roots"]):
        for root in roots:
            root_partition[root] = partition_idx

    duplicated = {}
    blockers = Counter()
    blocked_rows = {}
    unique_cpp_ids = {
        cpp_id
        for candidate in candidates
        for cpp_id in candidate["cpp_ids"]
        if cpp_id in tasks_by_id
    }
    unique_cost = sum(expected_cost(tasks_by_id[cpp_id], counts) for cpp_id in unique_cpp_ids)
    budget = unique_cost * args.replication_budget_ratio
    duplicate_expected_used = 0

    for candidate in candidates:
        partition = root_partition.get(candidate["root_cpp_id"], 0)
        for cpp_id in candidate["cpp_ids"]:
            task = tasks_by_id[cpp_id]
            expected = expected_cost(task, counts)
            consumers = [
                succ_id
                for succ_id in succs_by_id.get(cpp_id, [])
                if succ_id in root_partition and root_partition[succ_id] != partition
            ]
            if not consumers:
                continue
            reason = duplicate_block_reason(task, expected, args)
            if reason is None and duplicate_expected_used + expected > budget:
                reason = "replication_budget"
            if reason:
                blockers[reason] += expected
                blocked_rows.setdefault(reason, {
                    "reason": reason,
                    "blocked_expected_active_cost": 0,
                    "blocked_cpp_id_count": 0,
                    "cpp_ids": [],
                })
                blocked_rows[reason]["blocked_expected_active_cost"] += expected
                blocked_rows[reason]["blocked_cpp_id_count"] += 1
                if len(blocked_rows[reason]["cpp_ids"]) < args.top:
                    blocked_rows[reason]["cpp_ids"].append(cpp_id)
                continue
            if cpp_id not in duplicated:
                duplicated[cpp_id] = {
                    "cpp_id": cpp_id,
                    "super_id": task.get("super_id"),
                    "static_cost": static_cost(task),
                    "expected_active_cost": expected,
                    "source_partition": partition,
                    "consumer_partitions": sorted({root_partition[succ_id] for succ_id in consumers}),
                }
                duplicate_expected_used += expected

    duplicated_rows = sorted(duplicated.values(), key=lambda row: row["expected_active_cost"], reverse=True)
    copied_static = sum(row["static_cost"] for row in duplicated_rows)
    copied_expected = sum(row["expected_active_cost"] for row in duplicated_rows)
    replicated_expected = unique_cost + copied_expected
    partition_avg = active_assignment["expected_avg"]
    partition_max = active_assignment["expected_max"]
    estimated_barrier_reduction = len(duplicated_rows)
    return {
        "target_partitions": target,
        "unique_expected_cost": unique_cost,
        "replicated_expected_cost": replicated_expected,
        "duplicated_expected_active_cost": copied_expected,
        "replication_ratio": (replicated_expected / unique_cost) if unique_cost else 0,
        "copied_static_cost": copied_static,
        "copied_expected_active_cost": copied_expected,
        "duplicated_cpp_id_count": len(duplicated_rows),
        "duplicated_supernode_count": len({row["super_id"] for row in duplicated_rows}),
        "balanced_partition_expected_cost_max": partition_max,
        "balanced_partition_expected_cost_avg": partition_avg,
        "active_expected_imbalance_ratio": active_assignment["expected_imbalance_ratio"],
        "estimated_barrier_reduction": estimated_barrier_reduction,
        "active_weighted_assignment": active_assignment,
        "static_only_assignment": static_assignment,
        "top_replicated_nodes": duplicated_rows[:args.top],
        "top_blockers": sorted(blocked_rows.values(), key=lambda row: row["blocked_expected_active_cost"], reverse=True)[:args.top],
    }


def summarize_blockers(candidates):
    cost = Counter()
    count = Counter()
    for candidate in candidates:
        for blocked in candidate.get("blocked_units") or []:
            reason = blocked["reason"]
            cost[reason] += blocked.get("expected_active_cost", 0)
            count[reason] += 1
    return {
        "blocked_expected_active_cost": dict(cost.most_common()),
        "blocked_unit_count": dict(count.most_common()),
    }


def unique_candidate_expected_cost(candidates, tasks_by_id, counts):
    cpp_ids = {
        cpp_id
        for candidate in candidates
        for cpp_id in candidate["cpp_ids"]
        if cpp_id in tasks_by_id
    }
    return sum(expected_cost(tasks_by_id[cpp_id], counts) for cpp_id in cpp_ids)


def unique_candidate_static_cost(candidates, tasks_by_id):
    cpp_ids = {
        cpp_id
        for candidate in candidates
        for cpp_id in candidate["cpp_ids"]
        if cpp_id in tasks_by_id
    }
    return sum(static_cost(tasks_by_id[cpp_id]) for cpp_id in cpp_ids)


def decision(result, args):
    pure_expected = result["totals"]["pure_candidate_expected_active_cost"]
    total_expected = result["totals"]["total_expected_active_cost"]
    if not total_expected:
        return {
            "recommendation": "metadata_or_profile_repair",
            "reason": "profile_has_no_expected_active_cost",
            "safe_subset": "none",
        }
    share = pure_expected / total_expected
    best = max(result["partition_reports"], key=lambda row: row["estimated_barrier_reduction"], default=None)
    if best and share >= args.min_candidate_share and best["estimated_barrier_reduction"] > 0:
        return {
            "recommendation": "consider_23x_runtime_prototype_for_reported_pure_subset",
            "reason": "pure_macro_fiber_candidates_have_active_cost_and_bounded_replication",
            "safe_subset": "pure-only candidates with replicated nodes listed in top_replicated_nodes",
            "expected_benefit": {
                "candidate_share_of_total_expected_active_cost": share,
                "best_target_partitions": best["target_partitions"],
                "estimated_barrier_reduction": best["estimated_barrier_reduction"],
                "replication_ratio": best["replication_ratio"],
            },
        }
    return {
        "recommendation": "metadata_or_proof_repair_before_runtime",
        "reason": "insufficient pure active-weighted candidate work or no bounded replication opportunity",
        "safe_subset": "none",
    }


def markdown_table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    lines.extend("| " + " | ".join(str(value) for value in row) + " |" for row in rows)
    return "\n".join(lines)


def fmt(value):
    if isinstance(value, float):
        return f"{value:.6g}"
    return str(value)


def write_markdown(result, path):
    totals = result["totals"]
    decision_row = result["decision"]
    lines = [
        "# GSim-MT 22X Macro-Fiber RepCut Planner",
        "",
        "## Inputs",
        "",
        markdown_table(
            ["field", "value"],
            [
                ["schedule_json", result["inputs"]["schedule_json"]],
                ["profile_log", result["inputs"]["profile_log"]],
                ["source_commit", result["inputs"]["source_commit"]],
                ["profile_cycles", result["profile"]["cycles"]],
            ],
        ),
        "",
        "## Expected Active Cost",
        "",
        markdown_table(
            ["metric", "value"],
            [
                ["total_expected_active_cost", totals["total_expected_active_cost"]],
                ["pure_candidate_expected_active_cost", totals["pure_candidate_expected_active_cost"]],
                ["state_or_optimistic_candidate_expected_active_cost", totals["state_or_optimistic_candidate_expected_active_cost"]],
                ["static_cost_only_total", totals["static_cost_only_total"]],
                ["candidate_count", totals["candidate_count"]],
            ],
        ),
        "",
        "## Partition And Replication",
        "",
        markdown_table(
            [
                "target",
                "unique_expected",
                "replicated_expected",
                "ratio",
                "part_max",
                "part_avg",
                "active_imbalance",
                "static_expected_imbalance",
                "copied_expected",
                "dup_cpp_ids",
                "barrier_reduction",
            ],
            [
                [
                    row["target_partitions"],
                    row["unique_expected_cost"],
                    row["replicated_expected_cost"],
                    fmt(row["replication_ratio"]),
                    row["balanced_partition_expected_cost_max"],
                    fmt(row["balanced_partition_expected_cost_avg"]),
                    fmt(row["active_expected_imbalance_ratio"]),
                    fmt(row["static_only_assignment"]["expected_imbalance_ratio"]),
                    row["copied_expected_active_cost"],
                    row["duplicated_cpp_id_count"],
                    row["estimated_barrier_reduction"],
                ]
                for row in result["partition_reports"]
            ],
        ),
        "",
        "## Blockers",
        "",
        markdown_table(
            ["reason", "blocked_expected_active_cost", "blocked_unit_count"],
            [
                [
                    reason,
                    cost,
                    result["blockers"]["blocked_unit_count"].get(reason, 0),
                ]
                for reason, cost in result["blockers"]["blocked_expected_active_cost"].items()
            ],
        ),
        "",
        "## Top Candidates",
        "",
        markdown_table(
            ["root_cpp_id", "kind", "tasks", "static_cost", "expected_active_cost", "blocked"],
            [
                [
                    row["root_cpp_id"],
                    row["safety_kind"],
                    row["task_count"],
                    row["static_cost"],
                    row["expected_active_cost"],
                    ",".join(sorted({blocked["reason"] for blocked in row.get("blocked_units", [])})),
                ]
                for row in result["candidates"][:20]
            ],
        ),
        "",
        "## Decision",
        "",
        markdown_table(
            ["field", "value"],
            [
                ["recommendation", decision_row["recommendation"]],
                ["reason", decision_row["reason"]],
                ["safe_subset", decision_row["safe_subset"]],
                ["note", "Planner-only numbers; no runtime speedup claim is made in 22X."],
            ],
        ),
    ]
    path.write_text("\n".join(lines) + "\n")


def parse_targets(text):
    targets = []
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        value = int(item)
        expect(value > 0, f"target partition count must be positive: {value}")
        targets.append(value)
    expect(targets, "target partition list must not be empty")
    return targets


def summarize(schedule, profile_text, args):
    tasks = schedule.get("tasks")
    expect(isinstance(tasks, list) and tasks, "schedule JSON has no non-empty tasks array")
    counts = parse_task_counts(profile_text)
    profile = parse_profile_summary(profile_text)
    candidates, tasks_by_id, preds_by_id, succs_by_id = build_candidates(tasks, counts, profile, args.max_fiber_depth)
    targets = parse_targets(args.target_partitions)
    pure_candidates = [row for row in candidates if row["safety_kind"] == "pure-only"]
    state_or_optimistic = [row for row in candidates if row["safety_kind"] != "pure-only"]
    partition_reports = [
        plan_replication(candidates, tasks_by_id, preds_by_id, succs_by_id, counts, target, args)
        for target in targets
    ]
    total_expected = sum(expected_cost(task, counts) for task in tasks)
    pure_unique_expected = unique_candidate_expected_cost(pure_candidates, tasks_by_id, counts)
    state_or_optimistic_unique_expected = unique_candidate_expected_cost(state_or_optimistic, tasks_by_id, counts)
    all_candidate_unique_expected = unique_candidate_expected_cost(candidates, tasks_by_id, counts)
    result = {
        "format": "gsim.mt-22x-macro-fiber-repcut-planner.v1",
        "inputs": {
            "schedule_json": str(args.schedule_json),
            "profile_log": str(args.profile_log),
            "source_commit": git_commit(Path(__file__).resolve().parents[1]),
        },
        "budgets": {
            "max_fiber_depth": args.max_fiber_depth,
            "replication_budget_ratio": args.replication_budget_ratio,
            "max_duplicate_expected_cost": args.max_duplicate_expected_cost,
            "target_partitions": targets,
        },
        "profile": profile,
        "totals": {
            "task_count": len(tasks),
            "active_task_count": sum(1 for task in tasks if counts.get(task["cpp_id"], 0) > 0),
            "candidate_count": len(candidates),
            "pure_candidate_count": len(pure_candidates),
            "state_or_optimistic_candidate_count": len(state_or_optimistic),
            "total_expected_active_cost": total_expected,
            "candidate_unique_expected_active_cost": all_candidate_unique_expected,
            "candidate_expected_active_cost_with_overlap": sum(row["expected_active_cost"] for row in candidates),
            "pure_candidate_expected_active_cost": pure_unique_expected,
            "pure_candidate_expected_active_cost_with_overlap": sum(row["expected_active_cost"] for row in pure_candidates),
            "state_or_optimistic_candidate_expected_active_cost": state_or_optimistic_unique_expected,
            "state_or_optimistic_candidate_expected_active_cost_with_overlap": sum(row["expected_active_cost"] for row in state_or_optimistic),
            "candidate_unique_static_cost": unique_candidate_static_cost(candidates, tasks_by_id),
            "static_cost_only_total": sum(static_cost(task) for task in tasks),
        },
        "blockers": summarize_blockers(candidates),
        "candidates": candidates[: args.candidate_limit],
        "partition_reports": partition_reports,
    }
    result["decision"] = decision(result, args)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--schedule-json", type=Path, required=True)
    parser.add_argument("--profile-log", type=Path, required=True)
    parser.add_argument("--json-out", type=Path, required=True)
    parser.add_argument("--md-out", type=Path, required=True)
    parser.add_argument("--target-partitions", default=",".join(str(item) for item in TARGETS_DEFAULT))
    parser.add_argument("--max-fiber-depth", type=int, default=8)
    parser.add_argument("--replication-budget-ratio", type=float, default=0.10)
    parser.add_argument("--max-duplicate-expected-cost", type=int, default=1_000_000_000)
    parser.add_argument("--min-candidate-share", type=float, default=0.05)
    parser.add_argument("--candidate-limit", type=int, default=200)
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()

    expect(args.schedule_json.is_file(), f"missing schedule JSON {args.schedule_json}")
    expect(args.profile_log.is_file(), f"missing profile log {args.profile_log}")
    expect(args.max_fiber_depth >= 0, "--max-fiber-depth must be non-negative")
    expect(args.replication_budget_ratio >= 0, "--replication-budget-ratio must be non-negative")
    expect(args.max_duplicate_expected_cost >= 0, "--max-duplicate-expected-cost must be non-negative")

    schedule = read_json(args.schedule_json)
    profile_text = args.profile_log.read_text(errors="replace")
    result = summarize(schedule, profile_text, args)

    args.json_out.parent.mkdir(parents=True, exist_ok=True)
    args.md_out.parent.mkdir(parents=True, exist_ok=True)
    args.json_out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    write_markdown(result, args.md_out)
    print(
        "mt-22x-macro-fiber-repcut-planner ok "
        f"candidates={result['totals']['candidate_count']} "
        f"total_expected_active_cost={result['totals']['total_expected_active_cost']} "
        f"decision={result['decision']['recommendation']}"
    )


if __name__ == "__main__":
    main()
