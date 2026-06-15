#!/usr/bin/env python3
import json
import sys
from collections import defaultdict
from pathlib import Path


REQUIRED_BOUNDARY_FIELDS = {
    "has_state_update",
    "has_memory_write",
    "has_reset",
    "has_external",
    "has_special",
    "clock_names",
}

REQUIRED_REPCUT_FIELDS = {
    "is_source",
    "is_sink",
    "candidate_cost",
}

REQUIRED_STATE_UPDATE_FIELDS = {
    "has_state_update",
    "state_target_names",
    "state_target_count",
    "single_target",
    "rhs_timing_class",
    "rhs_timing_evidence",
    "rhs_reads_state_targets",
    "rhs_reads_same_cycle_target",
    "has_reset_behavior",
    "has_async_reset_behavior",
    "has_memory_or_dynamic_array",
    "has_external_or_special",
    "activation_fanout_count",
    "activation_can_use_delta",
    "candidate_kind",
    "block_reasons",
}

REQUIRED_STATE_UPDATE_GROUP_FIELDS = {
    "local_safe_candidate",
    "runtime_safe_candidate",
    "target_writer_count",
    "target_multi_target_writer_count",
    "target_writer_universe_complete",
    "target_writer_cpp_ids",
    "target_writer_conflict_kind",
    "target_writer_proof",
    "runtime_block_reasons",
}

VALID_TASK_KINDS = {"pure_compute", "serial"}
VALID_RHS_TIMING_CLASSES = {"precomputed", "old_state_only", "unknown"}
VALID_CANDIDATE_KINDS = {"safe_candidate", "needs_split", "blocked"}
VALID_TARGET_WRITER_CONFLICT_KINDS = {
    "none",
    "unique_writer",
    "multi_writer_unproven",
    "multi_target_unproven",
    "writer_universe_incomplete",
}
VALID_TARGET_WRITER_PROOFS = {"none", "target_unique_writer"}
TARGET_WRITER_ID_LIMIT = 16
VALID_RHS_TIMING_EVIDENCE = {
    "reg_src_commit_reads_next_state_object",
    "reg_dst_assign_tree_old_state_only",
    "no_state_update",
    "target_identity_ambiguous",
    "multiple_state_targets",
    "reset_or_activate_all",
    "memory_or_dynamic_array",
    "external_or_special_or_unknown",
    "rhs_reads_next_state_object",
    "rhs_reads_same_cycle_target",
    "rhs_dependency_unexpanded",
    "mixed_or_unproven_state_update",
}
PURE_FORBIDDEN_NODE_KINDS = {
    "NODE_INVALID",
    "NODE_REG_DST",
    "NODE_SPECIAL",
    "NODE_MEMORY",
    "NODE_READER",
    "NODE_WRITER",
    "NODE_READWRITER",
    "NODE_INFER",
    "NODE_REG_RESET",
    "NODE_EXT_IN",
    "NODE_EXT_OUT",
    "NODE_EXT",
}


def fail(message):
    print(f"mt-schedule-json check failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def expect_bool(obj, field, path):
    expect(field in obj, f"missing {path}.{field}")
    expect(isinstance(obj[field], bool), f"{path}.{field} must be bool")


def main():
    if len(sys.argv) != 2:
        fail("usage: check-mt-schedule-json.py <schedule.json>")

    path = Path(sys.argv[1])
    expect(path.is_file(), f"{path} does not exist")

    try:
        data = json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        fail(f"{path} is not valid JSON: {exc}")

    expect(data.get("format") == "gsim.mt-schedule.v1", "unexpected or missing format")
    tasks = data.get("tasks")
    expect(isinstance(tasks, list), "tasks must be a list")
    expect(tasks, "tasks must not be empty")

    seen_cpp_ids = set()
    edge_refs = []
    target_writer_cpp_ids = defaultdict(list)
    target_multi_target_writer_cpp_ids = defaultdict(list)
    for task in tasks:
        su = task.get("state_update") or {}
        if not su.get("has_state_update"):
            continue
        targets = su.get("state_target_names") or []
        for target in targets:
            target_writer_cpp_ids[target].append(task.get("cpp_id"))
            if len(targets) > 1:
                target_multi_target_writer_cpp_ids[target].append(task.get("cpp_id"))

    saw_state_update = False
    saw_reset = False
    saw_clock = False
    for scan_index, task in enumerate(tasks):
        path_prefix = f"tasks[{scan_index}]"
        expect(isinstance(task, dict), f"{path_prefix} must be an object")

        for field in (
            "cpp_id",
            "scan_index",
            "super_id",
            "super_type",
            "task_kind",
            "serial_reasons",
            "active_word",
            "active_mask",
            "node_kinds",
            "pred_cpp_ids",
            "succ_cpp_ids",
            "active_fanout",
            "boundary",
            "state_update",
            "state_update_group",
            "repcut",
        ):
            expect(field in task, f"missing {path_prefix}.{field}")

        cpp_id = task["cpp_id"]
        expect(isinstance(cpp_id, int) and cpp_id >= 0, f"{path_prefix}.cpp_id must be non-negative int")
        expect(cpp_id not in seen_cpp_ids, f"duplicate cpp_id {cpp_id}")
        seen_cpp_ids.add(cpp_id)
        expect(task["scan_index"] == scan_index, f"{path_prefix}.scan_index must match array order")
        expect(task["cpp_id"] == scan_index, f"{path_prefix}.cpp_id must match scan order for milestone 01")
        expect(isinstance(task["super_id"], int) and task["super_id"] > 0, f"{path_prefix}.super_id must be positive int")
        expect(isinstance(task["super_type"], str) and task["super_type"], f"{path_prefix}.super_type must be string")
        expect(task["task_kind"] in VALID_TASK_KINDS, f"{path_prefix}.task_kind must be one of {sorted(VALID_TASK_KINDS)}")
        expect(isinstance(task["serial_reasons"], list), f"{path_prefix}.serial_reasons must be list")
        for reason_index, reason in enumerate(task["serial_reasons"]):
            expect(isinstance(reason, str) and reason, f"{path_prefix}.serial_reasons[{reason_index}] must be non-empty string")
        if task["task_kind"] == "serial":
            expect(task["serial_reasons"], f"{path_prefix}.serial_reasons must not be empty for serial task")
        else:
            expect(not task["serial_reasons"], f"{path_prefix}.serial_reasons must be empty for pure_compute task")
        expect(isinstance(task["active_word"], int) and task["active_word"] >= 0, f"{path_prefix}.active_word must be non-negative int")
        expect(isinstance(task["active_mask"], str) and task["active_mask"].startswith("0x"), f"{path_prefix}.active_mask must be hex string")
        int(task["active_mask"], 16)

        node_kinds = task["node_kinds"]
        expect(isinstance(node_kinds, dict), f"{path_prefix}.node_kinds must be object")
        expect(node_kinds, f"{path_prefix}.node_kinds must not be empty")
        for name, count in node_kinds.items():
            expect(isinstance(name, str) and name.startswith("NODE_"), f"{path_prefix}.node_kinds key must be node kind")
            expect(isinstance(count, int) and count > 0, f"{path_prefix}.node_kinds[{name}] must be positive int")
        if task["task_kind"] == "pure_compute":
            expect(task["super_type"] == "SUPER_VALID",
                   f"{path_prefix}.task_kind pure_compute requires SUPER_VALID")
            forbidden_kinds = sorted(set(node_kinds).intersection(PURE_FORBIDDEN_NODE_KINDS))
            expect(not forbidden_kinds,
                   f"{path_prefix}.task_kind pure_compute has forbidden node kinds {forbidden_kinds}")

        for field in ("pred_cpp_ids", "succ_cpp_ids", "active_fanout"):
            values = task[field]
            expect(isinstance(values, list), f"{path_prefix}.{field} must be list")
            edge_refs.append((path_prefix, field, values))
            last = -1
            for value in values:
                expect(isinstance(value, int) and value >= 0, f"{path_prefix}.{field} values must be non-negative int")
                expect(value > last, f"{path_prefix}.{field} must be sorted and unique")
                last = value

        boundary = task["boundary"]
        expect(isinstance(boundary, dict), f"{path_prefix}.boundary must be object")
        expect(REQUIRED_BOUNDARY_FIELDS.issubset(boundary), f"{path_prefix}.boundary missing required fields")
        for field in REQUIRED_BOUNDARY_FIELDS - {"clock_names"}:
            expect_bool(boundary, field, f"{path_prefix}.boundary")
        expect(isinstance(boundary["clock_names"], list), f"{path_prefix}.boundary.clock_names must be list")
        for clock_name in boundary["clock_names"]:
            expect(isinstance(clock_name, str), f"{path_prefix}.boundary.clock_names values must be strings")
        saw_state_update = saw_state_update or boundary["has_state_update"]
        saw_reset = saw_reset or boundary["has_reset"]
        saw_clock = saw_clock or bool(boundary["clock_names"])
        if task["task_kind"] == "pure_compute":
            for field in ("has_state_update", "has_memory_write", "has_reset", "has_external", "has_special"):
                expect(not boundary[field], f"{path_prefix}.task_kind pure_compute requires boundary.{field}=false")

        state_update = task["state_update"]
        expect(isinstance(state_update, dict), f"{path_prefix}.state_update must be object")
        expect(REQUIRED_STATE_UPDATE_FIELDS.issubset(state_update),
               f"{path_prefix}.state_update missing required fields")
        for field in (
            "has_state_update",
            "single_target",
            "rhs_reads_same_cycle_target",
            "has_reset_behavior",
            "has_async_reset_behavior",
            "has_memory_or_dynamic_array",
            "has_external_or_special",
            "activation_can_use_delta",
        ):
            expect_bool(state_update, field, f"{path_prefix}.state_update")
        expect(state_update["has_state_update"] == boundary["has_state_update"],
               f"{path_prefix}.state_update.has_state_update must match boundary.has_state_update")
        expect(state_update["has_reset_behavior"] == boundary["has_reset"],
               f"{path_prefix}.state_update.has_reset_behavior must match boundary.has_reset")
        expect(isinstance(state_update["state_target_names"], list),
               f"{path_prefix}.state_update.state_target_names must be list")
        last_target = ""
        for target_index, target in enumerate(state_update["state_target_names"]):
            expect(isinstance(target, str) and target,
                   f"{path_prefix}.state_update.state_target_names[{target_index}] must be non-empty string")
            expect(target > last_target,
                   f"{path_prefix}.state_update.state_target_names must be sorted and unique")
            last_target = target
        expect(isinstance(state_update["state_target_count"], int) and state_update["state_target_count"] >= 0,
               f"{path_prefix}.state_update.state_target_count must be non-negative int")
        expect(state_update["state_target_count"] == len(state_update["state_target_names"]),
               f"{path_prefix}.state_update.state_target_count must match state_target_names length")
        expect(state_update["single_target"] == (state_update["state_target_count"] == 1),
               f"{path_prefix}.state_update.single_target must match target count")
        expect(state_update["rhs_timing_class"] in VALID_RHS_TIMING_CLASSES,
               f"{path_prefix}.state_update.rhs_timing_class must be one of {sorted(VALID_RHS_TIMING_CLASSES)}")
        expect(state_update["rhs_timing_evidence"] in VALID_RHS_TIMING_EVIDENCE,
               f"{path_prefix}.state_update.rhs_timing_evidence must be one of {sorted(VALID_RHS_TIMING_EVIDENCE)}")
        expect(isinstance(state_update["rhs_reads_state_targets"], list),
               f"{path_prefix}.state_update.rhs_reads_state_targets must be list")
        last_read_target = ""
        for target_index, target in enumerate(state_update["rhs_reads_state_targets"]):
            expect(isinstance(target, str) and target,
                   f"{path_prefix}.state_update.rhs_reads_state_targets[{target_index}] must be non-empty string")
            expect(target > last_read_target,
                   f"{path_prefix}.state_update.rhs_reads_state_targets must be sorted and unique")
            last_read_target = target
        expect(isinstance(state_update["activation_fanout_count"], int) and state_update["activation_fanout_count"] >= 0,
               f"{path_prefix}.state_update.activation_fanout_count must be non-negative int")
        expect(state_update["activation_fanout_count"] == len(task["active_fanout"]),
               f"{path_prefix}.state_update.activation_fanout_count must match active_fanout length")
        expect(state_update["candidate_kind"] in VALID_CANDIDATE_KINDS,
               f"{path_prefix}.state_update.candidate_kind must be one of {sorted(VALID_CANDIDATE_KINDS)}")
        expect(isinstance(state_update["block_reasons"], list),
               f"{path_prefix}.state_update.block_reasons must be list")
        seen_reasons = set()
        for reason_index, reason in enumerate(state_update["block_reasons"]):
            expect(isinstance(reason, str) and reason,
                   f"{path_prefix}.state_update.block_reasons[{reason_index}] must be non-empty string")
            expect(reason not in seen_reasons,
                   f"{path_prefix}.state_update.block_reasons must not contain duplicates")
            seen_reasons.add(reason)
        if state_update["candidate_kind"] == "safe_candidate":
            expect(state_update["has_state_update"],
                   f"{path_prefix}.state_update.safe_candidate requires has_state_update")
            expect(state_update["single_target"],
                   f"{path_prefix}.state_update.safe_candidate requires single_target")
            expect(state_update["rhs_timing_class"] != "unknown",
                   f"{path_prefix}.state_update.safe_candidate requires proven RHS timing")
            expect(not state_update["rhs_reads_same_cycle_target"],
                   f"{path_prefix}.state_update.safe_candidate must not read same-cycle target")
            expect(state_update["activation_can_use_delta"],
                   f"{path_prefix}.state_update.safe_candidate requires activation_can_use_delta")
            expect(not state_update["block_reasons"],
                   f"{path_prefix}.state_update.safe_candidate must not have block_reasons")
        else:
            expect(state_update["block_reasons"],
                   f"{path_prefix}.state_update non-safe candidate must explain block_reasons")
        if state_update["has_state_update"]:
            expect("state_update" in task["serial_reasons"],
                   f"{path_prefix}.state_update.has_state_update requires serial reason state_update")
            if state_update["rhs_timing_class"] == "unknown":
                expect(state_update["candidate_kind"] == "blocked",
                       f"{path_prefix}.state_update unknown RHS timing must be blocked")
                expect("rhs_timing_unknown" in state_update["block_reasons"],
                       f"{path_prefix}.state_update unknown RHS timing must add rhs_timing_unknown")
            if state_update["rhs_reads_same_cycle_target"]:
                expect(state_update["candidate_kind"] == "blocked",
                       f"{path_prefix}.state_update same-cycle target read must be blocked")
                expect("rhs_reads_same_cycle_target" in state_update["block_reasons"],
                       f"{path_prefix}.state_update same-cycle target read must add rhs_reads_same_cycle_target")
        else:
            expect(state_update["candidate_kind"] == "blocked",
                   f"{path_prefix}.state_update without state update must be blocked")
            expect("no_state_update" in state_update["block_reasons"],
                   f"{path_prefix}.state_update without state update must add no_state_update")

        state_update_group = task["state_update_group"]
        expect(isinstance(state_update_group, dict), f"{path_prefix}.state_update_group must be object")
        expect(REQUIRED_STATE_UPDATE_GROUP_FIELDS.issubset(state_update_group),
               f"{path_prefix}.state_update_group missing required fields")
        for field in ("local_safe_candidate", "runtime_safe_candidate"):
            expect_bool(state_update_group, field, f"{path_prefix}.state_update_group")
        expect_bool(state_update_group, "target_writer_universe_complete", f"{path_prefix}.state_update_group")
        expect(state_update_group["local_safe_candidate"] == (state_update["candidate_kind"] == "safe_candidate"),
               f"{path_prefix}.state_update_group.local_safe_candidate must mirror state_update.safe_candidate")
        expect(isinstance(state_update_group["target_writer_count"], int) and
               state_update_group["target_writer_count"] >= 0,
               f"{path_prefix}.state_update_group.target_writer_count must be non-negative int")
        expect(isinstance(state_update_group["target_multi_target_writer_count"], int) and
               state_update_group["target_multi_target_writer_count"] >= 0,
               f"{path_prefix}.state_update_group.target_multi_target_writer_count must be non-negative int")
        expect(isinstance(state_update_group["target_writer_cpp_ids"], list),
               f"{path_prefix}.state_update_group.target_writer_cpp_ids must be list")
        last_writer_cpp_id = -1
        for value in state_update_group["target_writer_cpp_ids"]:
            expect(isinstance(value, int) and value >= 0,
                   f"{path_prefix}.state_update_group.target_writer_cpp_ids values must be non-negative int")
            expect(value > last_writer_cpp_id,
                   f"{path_prefix}.state_update_group.target_writer_cpp_ids must be sorted and unique")
            last_writer_cpp_id = value
        expect(state_update_group["target_writer_count"] >= len(state_update_group["target_writer_cpp_ids"]),
               f"{path_prefix}.state_update_group.target_writer_count must cover emitted writer ids")
        if state_update["has_state_update"] and state_update["state_target_count"] == 1:
            target = state_update["state_target_names"][0]
            expected_writer_ids = sorted(target_writer_cpp_ids[target])
            expected_multi_target_writer_ids = sorted(target_multi_target_writer_cpp_ids[target])
            expect(state_update_group["target_writer_count"] == len(expected_writer_ids),
                   f"{path_prefix}.state_update_group.target_writer_count must include every single-target and multi-target writer for {target}")
            expect(state_update_group["target_multi_target_writer_count"] == len(expected_multi_target_writer_ids),
                   f"{path_prefix}.state_update_group.target_multi_target_writer_count must include every multi-target writer for {target}")
            expect(state_update_group["target_writer_cpp_ids"] == expected_writer_ids[:TARGET_WRITER_ID_LIMIT],
                   f"{path_prefix}.state_update_group.target_writer_cpp_ids must be the capped full writer universe for {target}")
            expect(cpp_id in state_update_group["target_writer_cpp_ids"],
                   f"{path_prefix}.state_update_group.target_writer_cpp_ids must include this task")
            expect(state_update_group["target_writer_count"] >= 1,
                   f"{path_prefix}.state_update_group state updates require at least one writer")
            if expected_multi_target_writer_ids:
                expect(not state_update_group["runtime_safe_candidate"],
                       f"{path_prefix}.state_update_group target {target} overlaps multi-target writers {expected_multi_target_writer_ids} and must not be runtime-safe")
                expect(
                    "target_multi_writer_unproven" in state_update_group["runtime_block_reasons"] or
                    "target_multi_target_writer_unproven" in state_update_group["runtime_block_reasons"],
                    f"{path_prefix}.state_update_group target {target} overlapping multi-target writers must record a runtime blocker"
                )
        elif state_update["has_state_update"] and state_update["state_target_count"] > 1:
            expect(state_update_group["target_writer_count"] == 0,
                   f"{path_prefix}.state_update_group multi-target updates should not expand writer counts")
            expect(state_update_group["target_multi_target_writer_count"] == 0,
                   f"{path_prefix}.state_update_group multi-target updates should not emit scalar multi-target writer counts")
            expect(not state_update_group["target_writer_cpp_ids"],
                   f"{path_prefix}.state_update_group multi-target updates should not list writer ids")
        else:
            expect(state_update_group["target_writer_count"] == 0,
                   f"{path_prefix}.state_update_group non-state updates must have zero writer count")
            expect(state_update_group["target_multi_target_writer_count"] == 0,
                   f"{path_prefix}.state_update_group non-state updates must have zero multi-target writer count")
            expect(not state_update_group["target_writer_cpp_ids"],
                   f"{path_prefix}.state_update_group non-state updates must not list writer ids")
        expect(state_update_group["target_writer_conflict_kind"] in VALID_TARGET_WRITER_CONFLICT_KINDS,
               f"{path_prefix}.state_update_group.target_writer_conflict_kind must be one of {sorted(VALID_TARGET_WRITER_CONFLICT_KINDS)}")
        expect(state_update_group["target_writer_proof"] in VALID_TARGET_WRITER_PROOFS,
               f"{path_prefix}.state_update_group.target_writer_proof must be one of {sorted(VALID_TARGET_WRITER_PROOFS)}")
        if state_update_group["target_writer_count"] == 0:
            expect(state_update_group["target_writer_conflict_kind"] in {"none", "multi_target_unproven"},
                   f"{path_prefix}.state_update_group zero writers must use conflict kind none or multi_target_unproven")
            expect(state_update_group["target_writer_proof"] == "none",
                   f"{path_prefix}.state_update_group zero writers must use proof none")
        elif state_update_group["target_writer_count"] == 1:
            if state_update_group["target_writer_universe_complete"]:
                expect(state_update_group["target_writer_conflict_kind"] == "unique_writer",
                       f"{path_prefix}.state_update_group single writer with complete universe must use unique_writer")
                expect(state_update_group["target_writer_proof"] == "target_unique_writer",
                       f"{path_prefix}.state_update_group single writer must use target_unique_writer proof")
            else:
                expect(state_update_group["target_writer_conflict_kind"] == "writer_universe_incomplete",
                       f"{path_prefix}.state_update_group incomplete writer universe must block unique writer proof")
                expect(state_update_group["target_writer_proof"] == "none",
                       f"{path_prefix}.state_update_group incomplete writer universe must not claim proof")
        else:
            expect(state_update_group["target_writer_conflict_kind"] == "multi_writer_unproven",
                   f"{path_prefix}.state_update_group multi-writer targets must stay unproven")
            expect(state_update_group["target_writer_proof"] == "none",
                   f"{path_prefix}.state_update_group multi-writer proof must be none")
        if state_update_group["target_writer_conflict_kind"] == "writer_universe_incomplete":
            expect("target_writer_universe_incomplete" in state_update_group["runtime_block_reasons"],
                   f"{path_prefix}.state_update_group incomplete writer universe must record target_writer_universe_incomplete")
        if state_update["has_state_update"] and state_update["state_target_count"] > 1:
            expect(state_update_group["target_writer_conflict_kind"] == "multi_target_unproven",
                   f"{path_prefix}.state_update_group multi-target updates must use multi_target_unproven")
            expect("target_multi_target_unproven" in state_update_group["runtime_block_reasons"],
                   f"{path_prefix}.state_update_group multi-target updates must record target_multi_target_unproven")
        expect(isinstance(state_update_group["runtime_block_reasons"], list),
               f"{path_prefix}.state_update_group.runtime_block_reasons must be list")
        seen_runtime_reasons = set()
        for reason_index, reason in enumerate(state_update_group["runtime_block_reasons"]):
            expect(isinstance(reason, str) and reason,
                   f"{path_prefix}.state_update_group.runtime_block_reasons[{reason_index}] must be non-empty string")
            expect(reason not in seen_runtime_reasons,
                   f"{path_prefix}.state_update_group.runtime_block_reasons must not contain duplicates")
            seen_runtime_reasons.add(reason)
        if state_update_group["runtime_safe_candidate"]:
            expect(state_update_group["local_safe_candidate"],
                   f"{path_prefix}.state_update_group.runtime_safe_candidate requires local_safe_candidate")
            expect(state_update_group["target_writer_conflict_kind"] == "unique_writer",
                   f"{path_prefix}.state_update_group.runtime_safe_candidate requires unique writer")
            expect(not state_update_group["runtime_block_reasons"],
                   f"{path_prefix}.state_update_group.runtime_safe_candidate must not have runtime blockers")
        else:
            expect(state_update_group["runtime_block_reasons"],
                   f"{path_prefix}.state_update_group non-runtime-safe candidate must explain runtime blockers")
        if state_update_group["local_safe_candidate"] and state_update_group["target_writer_count"] > 1:
            expect(not state_update_group["runtime_safe_candidate"],
                   f"{path_prefix}.state_update_group duplicate-writer local-safe task must not be runtime-safe")
            expect("target_multi_writer_unproven" in state_update_group["runtime_block_reasons"],
                   f"{path_prefix}.state_update_group duplicate writers must add target_multi_writer_unproven")

        repcut = task["repcut"]
        expect(isinstance(repcut, dict), f"{path_prefix}.repcut must be object")
        expect(REQUIRED_REPCUT_FIELDS.issubset(repcut), f"{path_prefix}.repcut missing required fields")
        expect_bool(repcut, "is_source", f"{path_prefix}.repcut")
        expect_bool(repcut, "is_sink", f"{path_prefix}.repcut")
        expect(repcut["candidate_cost"] is None or isinstance(repcut["candidate_cost"], int),
               f"{path_prefix}.repcut.candidate_cost must be null or int")
        if task["task_kind"] == "pure_compute":
            expect(isinstance(repcut["candidate_cost"], int) and repcut["candidate_cost"] >= 0,
                   f"{path_prefix}.repcut.candidate_cost must be non-negative int for pure_compute")
        else:
            expect(repcut["candidate_cost"] is None,
                   f"{path_prefix}.repcut.candidate_cost must be null for serial task")

    for path_prefix, field, values in edge_refs:
        for value in values:
            expect(value in seen_cpp_ids, f"{path_prefix}.{field} references missing cpp_id {value}")

    if path.name == "repro-usefulreset_mt_schedule.json":
        expect(saw_state_update, "repro-usefulreset schedule must expose state-update boundary evidence")
        expect(saw_reset, "repro-usefulreset schedule must expose reset boundary evidence")
        expect(saw_clock, "repro-usefulreset schedule must expose clock boundary evidence")

    print(f"checked {len(tasks)} mt schedule tasks in {path}")


if __name__ == "__main__":
    main()
