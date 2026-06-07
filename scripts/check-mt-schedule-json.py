#!/usr/bin/env python3
import json
import sys
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

VALID_TASK_KINDS = {"pure_compute", "serial"}
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
