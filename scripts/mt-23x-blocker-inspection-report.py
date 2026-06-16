#!/usr/bin/env python3
import argparse
import json
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path


def fail(message):
    print(f"mt-23x-blocker-inspection-report failed: {message}", file=sys.stderr)
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
    for item in match.group(1).strip().split(","):
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
    return {"cycles": cycles, "main": main}


def git_commit(repo_root):
    try:
        result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo_root, text=True, capture_output=True, check=True)
        return result.stdout.strip()
    except Exception:
        return "unknown"


def static_cost(task):
    repcut = task.get("repcut") or {}
    cost = repcut.get("candidate_cost")
    if isinstance(cost, int) and cost > 0:
        return cost
    node_kinds = task.get("node_kinds") or {}
    return max(1, sum(value for value in node_kinds.values() if isinstance(value, int)))


def expected_cost(task, counts):
    return static_cost(task) * counts.get(task["cpp_id"], 0)


def state_update(task):
    return task.get("state_update") or {}


def state_update_group(task):
    return task.get("state_update_group") or {}


def boundary(task):
    return task.get("boundary") or {}


def has_reason(task, reason):
    return reason in (task.get("serial_reasons") or [])


def main_blocker(task):
    b = boundary(task)
    su = state_update(task)
    if task.get("super_type") == "SUPER_EXTMOD" or b.get("has_external") or has_reason(task, "external"):
        return "external"
    if b.get("has_special") or has_reason(task, "special"):
        return "serial_side_effect"
    if b.get("has_memory_write") or has_reason(task, "memory_write") or has_reason(task, "memory_read_unsupported") or has_reason(task, "array_or_dynamic_index") or su.get("has_memory_or_dynamic_array"):
        return "memory"
    if b.get("has_reset") or has_reason(task, "reset") or has_reason(task, "async_reset") or su.get("has_reset_behavior") or su.get("has_async_reset_behavior"):
        return "reset"
    if b.get("has_state_update") or su.get("has_state_update") or has_reason(task, "state_update"):
        if state_update_group(task).get("runtime_safe_candidate"):
            return None
        return "state_update_proof"
    if task.get("task_kind") != "pure_compute":
        return "serial_side_effect"
    return None


def state_update_subreason(task):
    su = state_update(task)
    group = state_update_group(task)
    runtime_blocks = set(group.get("runtime_block_reasons") or [])
    block_reasons = set(su.get("block_reasons") or [])
    if group.get("runtime_safe_candidate"):
        return "state_update_runtime_safe"
    if group.get("local_safe_candidate") and "target_multi_writer_unproven" in runtime_blocks:
        return "state_update_local_safe_duplicate_writer_blocked"
    if su.get("rhs_timing_class") == "unknown" or "rhs_timing_unknown" in block_reasons:
        return "state_update_unknown_rhs_timing"
    if su.get("state_target_count", 0) > 1 or "multiple_state_targets" in block_reasons:
        return "state_update_multi_target_writer_unproven"
    if "target_multi_target_writer_unproven" in runtime_blocks:
        return "state_update_multi_target_writer_unproven"
    if "target_multi_writer_unproven" in runtime_blocks:
        return "state_update_duplicate_writer_blocked"
    if su.get("rhs_reads_same_cycle_target"):
        return "state_update_same_cycle_target_read"
    if not group.get("local_safe_candidate"):
        return "state_update_not_local_safe"
    return "state_update_runtime_proof_missing"


def memory_subreason(task):
    su = state_update(task)
    if has_reason(task, "memory_write") or boundary(task).get("has_memory_write"):
        return "memory_write_serial"
    if has_reason(task, "memory_read_unsupported"):
        return "memory_read_unsupported"
    if has_reason(task, "array_or_dynamic_index"):
        return "memory_dynamic_or_array_index"
    if su.get("has_memory_or_dynamic_array"):
        return "memory_model_or_dynamic_array_unsupported"
    return "memory_unknown_model"


def reset_subreason(task):
    su = state_update(task)
    if has_reason(task, "async_reset") or su.get("has_async_reset_behavior") or task.get("super_type") == "SUPER_ASYNC_RESET":
        return "async_reset_observation_point"
    if su.get("has_state_update") or boundary(task).get("has_state_update") or has_reason(task, "state_update"):
        return "reset_commit_or_side_effect"
    return "reset_source_compute_boundary"


def blocker_subreason(task):
    reason = main_blocker(task)
    if reason == "state_update_proof":
        return state_update_subreason(task)
    if reason == "memory":
        return memory_subreason(task)
    if reason == "reset":
        return reset_subreason(task)
    if reason == "external":
        return "external_or_blackbox"
    if reason == "serial_side_effect":
        if boundary(task).get("has_special") or has_reason(task, "special"):
            return "special_statement_or_assert"
        return "serial_side_effect_unclassified"
    return "not_blocked"


def must_remain_serial_reason(task, reason, subreason):
    if reason == "state_update_proof":
        group = state_update_group(task)
        if group.get("runtime_safe_candidate"):
            return "runtime-safe by emitted writer-universe proof"
        return f"state update lacks runtime-safe proof: {subreason}"
    if subreason == "memory_write_serial":
        return "memory write remains serial without an independent buffered-memory semantics proof"
    if reason == "memory":
        return f"memory access remains serial: {subreason}"
    if subreason == "async_reset_observation_point":
        return "async reset must preserve original two-observation-point behavior"
    if reason == "reset":
        return f"reset boundary remains serial: {subreason}"
    if reason == "external":
        return "external or black-box behavior has no local purity proof"
    if reason == "serial_side_effect":
        return f"serial side effect remains serial: {subreason}"
    return "not blocked by 23X metadata inspection"


def source_facts(task):
    su = state_update(task)
    group = state_update_group(task)
    b = boundary(task)
    return {
        "serial_reasons": task.get("serial_reasons") or [],
        "node_kinds": task.get("node_kinds") or {},
        "boundary": {
            "has_state_update": b.get("has_state_update", False),
            "has_memory_write": b.get("has_memory_write", False),
            "has_reset": b.get("has_reset", False),
            "has_external": b.get("has_external", False),
            "has_special": b.get("has_special", False),
            "clock_names": b.get("clock_names") or [],
        },
        "state_update": {
            "has_state_update": su.get("has_state_update", False),
            "state_target_count": su.get("state_target_count", 0),
            "state_target_names_sample": (su.get("state_target_names") or [])[:8],
            "rhs_timing_class": su.get("rhs_timing_class", ""),
            "rhs_timing_evidence": su.get("rhs_timing_evidence", ""),
            "rhs_reads_same_cycle_target": su.get("rhs_reads_same_cycle_target", False),
            "has_reset_behavior": su.get("has_reset_behavior", False),
            "has_async_reset_behavior": su.get("has_async_reset_behavior", False),
            "has_memory_or_dynamic_array": su.get("has_memory_or_dynamic_array", False),
            "has_external_or_special": su.get("has_external_or_special", False),
            "candidate_kind": su.get("candidate_kind", ""),
            "block_reasons": su.get("block_reasons") or [],
        },
        "state_update_group": {
            "local_safe_candidate": group.get("local_safe_candidate", False),
            "runtime_safe_candidate": group.get("runtime_safe_candidate", False),
            "target_writer_count": group.get("target_writer_count", 0),
            "target_multi_target_writer_count": group.get("target_multi_target_writer_count", 0),
            "target_writer_universe_complete": group.get("target_writer_universe_complete", False),
            "target_writer_conflict_kind": group.get("target_writer_conflict_kind", ""),
            "target_writer_proof": group.get("target_writer_proof", ""),
            "runtime_block_reasons": group.get("runtime_block_reasons") or [],
        },
    }


def safety_kind(task):
    su = state_update(task)
    group = state_update_group(task)
    if su.get("has_state_update") or "state_update" in (task.get("serial_reasons") or []):
        if group.get("runtime_safe_candidate"):
            return "runtime-safe-state-rooted"
        if group.get("local_safe_candidate"):
            return "local-safe-state-rooted"
        return "optimistic-only"
    if task.get("task_kind") == "pure_compute":
        return "pure-only"
    return "optimistic-only"


def row_for_task(task, counts):
    reason = main_blocker(task)
    subreason = blocker_subreason(task)
    return {
        "cpp_id": task["cpp_id"],
        "super_id": task.get("super_id"),
        "task_kind": task.get("task_kind"),
        "super_type": task.get("super_type"),
        "blocker_reason": reason or "runtime_safe_or_pure",
        "blocker_subreason": subreason,
        "static_cost": static_cost(task),
        "active_count": counts.get(task["cpp_id"], 0),
        "expected_active_cost": expected_cost(task, counts),
        "safety_kind": safety_kind(task),
        "source_facts": source_facts(task),
        "missing_proof_field": missing_proof_field(task, reason, subreason),
        "must_remain_serial_reason": must_remain_serial_reason(task, reason, subreason),
    }


def missing_proof_field(task, reason, subreason):
    if reason == "state_update_proof":
        if subreason == "state_update_local_safe_duplicate_writer_blocked":
            return "state_update_group.runtime_block_reasons[target_multi_writer_unproven]"
        if subreason == "state_update_unknown_rhs_timing":
            return "state_update.rhs_timing_class"
        if subreason == "state_update_multi_target_writer_unproven":
            return "state_update_group.target_writer_conflict_kind"
        if subreason == "state_update_not_local_safe":
            return "state_update.candidate_kind"
        return "state_update_group.runtime_safe_candidate"
    if reason == "memory":
        return "memory_independence_or_buffered_semantics_proof"
    if reason == "reset":
        return "reset_observation_point_preservation_proof"
    if reason == "external":
        return "external_purity_or_blackbox_semantics_proof"
    if reason == "serial_side_effect":
        return "side_effect_purity_proof"
    return ""


def summarize(schedule, profile_text, args):
    tasks = schedule.get("tasks")
    expect(isinstance(tasks, list) and tasks, "schedule JSON has no non-empty tasks array")
    counts = parse_task_counts(profile_text)
    profile = parse_profile_summary(profile_text)

    rows = [row_for_task(task, counts) for task in tasks]
    blocked_rows = [row for row in rows if row["blocker_reason"] != "runtime_safe_or_pure"]
    blocked_rows.sort(key=lambda row: (row["expected_active_cost"], row["static_cost"], row["cpp_id"]), reverse=True)

    cost_by_reason = Counter()
    cost_by_subreason = Counter()
    count_by_subreason = Counter()
    for row in blocked_rows:
        cost_by_reason[row["blocker_reason"]] += row["expected_active_cost"]
        cost_by_subreason[row["blocker_subreason"]] += row["expected_active_cost"]
        count_by_subreason[row["blocker_subreason"]] += 1

    runtime_safe_rows = [
        row for row in rows
        if row["safety_kind"] == "runtime-safe-state-rooted"
    ]
    optimistic_runtime_safe_rows = [
        row for row in runtime_safe_rows
        if row["source_facts"]["state_update_group"].get("runtime_safe_candidate")
        and not row["source_facts"]["state_update_group"].get("local_safe_candidate")
    ]
    total_expected = sum(expected_cost(task, counts) for task in tasks)
    result = {
        "format": "gsim.mt-23x-blocker-inspection.v1",
        "inputs": {
            "schedule_json": str(args.schedule_json),
            "profile_log": str(args.profile_log),
            "source_commit": git_commit(Path(__file__).resolve().parents[1]),
        },
        "profile": profile,
        "totals": {
            "task_count": len(tasks),
            "active_task_count": sum(1 for task in tasks if counts.get(task["cpp_id"], 0) > 0),
            "total_expected_active_cost": total_expected,
            "blocked_expected_active_cost": sum(row["expected_active_cost"] for row in blocked_rows),
            "runtime_safe_expected_active_cost": sum(row["expected_active_cost"] for row in runtime_safe_rows),
            "optimistic_runtime_safe_expected_active_cost": sum(row["expected_active_cost"] for row in optimistic_runtime_safe_rows),
        },
        "cost_by_blocker_reason": dict(cost_by_reason.most_common()),
        "cost_by_blocker_subreason": dict(cost_by_subreason.most_common()),
        "count_by_blocker_subreason": dict(count_by_subreason.most_common()),
        "top_blocked_candidates": blocked_rows[:args.top],
        "runtime_safe_candidates": sorted(runtime_safe_rows, key=lambda row: row["expected_active_cost"], reverse=True)[:args.top],
    }
    return result


def markdown_table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    lines.extend("| " + " | ".join(str(value) for value in row) + " |" for row in rows)
    return "\n".join(lines)


def write_markdown(result, path):
    totals = result["totals"]
    lines = [
        "# GSim-MT 23X Blocker Inspection Report",
        "",
        "## Totals",
        "",
        markdown_table(
            ["metric", "value"],
            [
                ["total_expected_active_cost", totals["total_expected_active_cost"]],
                ["blocked_expected_active_cost", totals["blocked_expected_active_cost"]],
                ["runtime_safe_expected_active_cost", totals["runtime_safe_expected_active_cost"]],
                ["optimistic_runtime_safe_expected_active_cost", totals["optimistic_runtime_safe_expected_active_cost"]],
            ],
        ),
        "",
        "## Blocker Subreasons",
        "",
        markdown_table(
            ["subreason", "expected_active_cost", "task_count"],
            [
                [subreason, cost, result["count_by_blocker_subreason"].get(subreason, 0)]
                for subreason, cost in result["cost_by_blocker_subreason"].items()
            ],
        ),
        "",
        "## Top Blocked Candidates",
        "",
        markdown_table(
            ["cpp_id", "super_id", "reason", "subreason", "static", "active", "expected", "missing_proof"],
            [
                [
                    row["cpp_id"],
                    row["super_id"],
                    row["blocker_reason"],
                    row["blocker_subreason"],
                    row["static_cost"],
                    row["active_count"],
                    row["expected_active_cost"],
                    row["missing_proof_field"],
                ]
                for row in result["top_blocked_candidates"][:40]
            ],
        ),
        "",
        "This is a report-only metadata inspection. It does not implement a runtime code path.",
    ]
    path.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--schedule-json", type=Path, required=True)
    parser.add_argument("--profile-log", type=Path, required=True)
    parser.add_argument("--json-out", type=Path, required=True)
    parser.add_argument("--md-out", type=Path, required=True)
    parser.add_argument("--top", type=int, default=100)
    args = parser.parse_args()

    expect(args.schedule_json.is_file(), f"missing schedule JSON {args.schedule_json}")
    expect(args.profile_log.is_file(), f"missing profile log {args.profile_log}")
    schedule = read_json(args.schedule_json)
    profile_text = args.profile_log.read_text(errors="replace")
    result = summarize(schedule, profile_text, args)
    args.json_out.parent.mkdir(parents=True, exist_ok=True)
    args.md_out.parent.mkdir(parents=True, exist_ok=True)
    args.json_out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    write_markdown(result, args.md_out)
    print(
        "mt-23x-blocker-inspection-report ok "
        f"blocked_expected_active_cost={result['totals']['blocked_expected_active_cost']} "
        f"runtime_safe_expected_active_cost={result['totals']['runtime_safe_expected_active_cost']}"
    )


if __name__ == "__main__":
    main()
