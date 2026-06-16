#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message):
    print(f"mt-23x-blocker-inspection-report check failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def base_task(cpp_id, serial_reasons, *, node_kinds=None, boundary=None, state_update=None, group=None):
    boundary = boundary or {
        "has_state_update": "state_update" in serial_reasons,
        "has_memory_write": False,
        "has_reset": False,
        "has_external": False,
        "has_special": False,
        "clock_names": ["clock"],
    }
    state_update = state_update or {
        "has_state_update": "state_update" in serial_reasons,
        "state_target_names": [f"r{cpp_id}"] if "state_update" in serial_reasons else [],
        "state_target_count": 1 if "state_update" in serial_reasons else 0,
        "single_target": "state_update" in serial_reasons,
        "rhs_timing_class": "unknown",
        "rhs_timing_evidence": "test",
        "rhs_reads_state_targets": [],
        "rhs_reads_same_cycle_target": False,
        "has_reset_behavior": False,
        "has_async_reset_behavior": False,
        "has_memory_or_dynamic_array": False,
        "has_external_or_special": False,
        "activation_fanout_count": 0,
        "activation_can_use_delta": False,
        "candidate_kind": "blocked",
        "block_reasons": [],
    }
    group = group or {
        "local_safe_candidate": False,
        "runtime_safe_candidate": False,
        "target_writer_count": 0,
        "target_multi_target_writer_count": 0,
        "target_writer_universe_complete": True,
        "target_writer_cpp_ids": [],
        "target_writer_conflict_kind": "none",
        "target_writer_proof": "none",
        "runtime_block_reasons": [],
    }
    return {
        "cpp_id": cpp_id,
        "scan_index": cpp_id,
        "super_id": 100 + cpp_id,
        "super_type": "SUPER_VALID",
        "task_kind": "serial",
        "serial_reasons": serial_reasons,
        "active_word": 0,
        "active_mask": hex(1 << cpp_id),
        "node_kinds": node_kinds or {"NODE_REG_DST": 1},
        "pred_cpp_ids": [],
        "succ_cpp_ids": [],
        "active_fanout": [],
        "boundary": boundary,
        "state_update": state_update,
        "state_update_group": group,
        "repcut": {"candidate_cost": 10},
    }


def profile(counts):
    body = ",".join(f"{cpp_id}:{count}" for cpp_id, count in sorted(counts.items()))
    return (
        "[mt-profile] helper_mode=mt worker_count=4 worker_pool=1 min_batch_tasks=1 "
        "max_worker_count=4 cycles=100 active_word_count=1 serial_tasks=0 pure_tasks=0 "
        "pure_batch_count=0 true_parallel_batch_count=0 skipped_fake_parallel_batch_count=0 "
        "serial_fast_task_count=0 batch_wall_ns=0 true_parallel_wall_ns=0 serial_wall_ns=0 "
        "merge_wall_ns=0 total_step_ns=0\n"
        f"[mt-profile] task_cpp_ids={body}\n"
    )


def fixture_schedule():
    local_dup = base_task(
        0,
        ["state_update"],
        state_update={
            "has_state_update": True,
            "state_target_names": ["dup"],
            "state_target_count": 1,
            "single_target": True,
            "rhs_timing_class": "old_state_only",
            "rhs_timing_evidence": "single_target_old_state",
            "rhs_reads_state_targets": [],
            "rhs_reads_same_cycle_target": False,
            "has_reset_behavior": False,
            "has_async_reset_behavior": False,
            "has_memory_or_dynamic_array": False,
            "has_external_or_special": False,
            "activation_fanout_count": 1,
            "activation_can_use_delta": True,
            "candidate_kind": "safe_candidate",
            "block_reasons": [],
        },
        group={
            "local_safe_candidate": True,
            "runtime_safe_candidate": False,
            "target_writer_count": 2,
            "target_multi_target_writer_count": 0,
            "target_writer_universe_complete": True,
            "target_writer_cpp_ids": [0, 7],
            "target_writer_conflict_kind": "multi_writer_unproven",
            "target_writer_proof": "none",
            "runtime_block_reasons": ["target_multi_writer_unproven"],
        },
    )
    unknown_rhs = base_task(
        1,
        ["state_update"],
        state_update={
            "has_state_update": True,
            "state_target_names": ["unknown_rhs"],
            "state_target_count": 1,
            "single_target": True,
            "rhs_timing_class": "unknown",
            "rhs_timing_evidence": "rhs_dependency_unexpanded",
            "rhs_reads_state_targets": [],
            "rhs_reads_same_cycle_target": False,
            "has_reset_behavior": False,
            "has_async_reset_behavior": False,
            "has_memory_or_dynamic_array": False,
            "has_external_or_special": False,
            "activation_fanout_count": 0,
            "activation_can_use_delta": False,
            "candidate_kind": "blocked",
            "block_reasons": ["rhs_timing_unknown", "activation_delta_unproven"],
        },
        group={
            "local_safe_candidate": False,
            "runtime_safe_candidate": False,
            "target_writer_count": 1,
            "target_multi_target_writer_count": 0,
            "target_writer_universe_complete": True,
            "target_writer_cpp_ids": [1],
            "target_writer_conflict_kind": "unique_writer",
            "target_writer_proof": "target_unique_writer",
            "runtime_block_reasons": ["not_local_safe_candidate"],
        },
    )
    runtime_safe = base_task(
        2,
        ["state_update"],
        state_update={
            "has_state_update": True,
            "state_target_names": ["safe"],
            "state_target_count": 1,
            "single_target": True,
            "rhs_timing_class": "old_state_only",
            "rhs_timing_evidence": "single_target_old_state",
            "rhs_reads_state_targets": [],
            "rhs_reads_same_cycle_target": False,
            "has_reset_behavior": False,
            "has_async_reset_behavior": False,
            "has_memory_or_dynamic_array": False,
            "has_external_or_special": False,
            "activation_fanout_count": 0,
            "activation_can_use_delta": True,
            "candidate_kind": "safe_candidate",
            "block_reasons": [],
        },
        group={
            "local_safe_candidate": True,
            "runtime_safe_candidate": True,
            "target_writer_count": 1,
            "target_multi_target_writer_count": 0,
            "target_writer_universe_complete": True,
            "target_writer_cpp_ids": [2],
            "target_writer_conflict_kind": "unique_writer",
            "target_writer_proof": "target_unique_writer",
            "runtime_block_reasons": [],
        },
    )
    memory_read = base_task(
        3,
        ["memory_read_unsupported"],
        node_kinds={"NODE_READER": 1},
        boundary={"has_state_update": False, "has_memory_write": False, "has_reset": False, "has_external": False, "has_special": False, "clock_names": ["clock"]},
    )
    memory_write = base_task(
        4,
        ["memory_write"],
        node_kinds={"NODE_WRITER": 1},
        boundary={"has_state_update": False, "has_memory_write": True, "has_reset": False, "has_external": False, "has_special": False, "clock_names": ["clock"]},
    )
    reset_commit = base_task(
        5,
        ["state_update", "reset"],
        boundary={"has_state_update": True, "has_memory_write": False, "has_reset": True, "has_external": False, "has_special": False, "clock_names": ["clock"]},
        state_update={
            "has_state_update": True,
            "state_target_names": ["rr"],
            "state_target_count": 1,
            "single_target": True,
            "rhs_timing_class": "unknown",
            "rhs_timing_evidence": "reset_or_activate_all",
            "rhs_reads_state_targets": [],
            "rhs_reads_same_cycle_target": False,
            "has_reset_behavior": True,
            "has_async_reset_behavior": False,
            "has_memory_or_dynamic_array": False,
            "has_external_or_special": False,
            "activation_fanout_count": 1,
            "activation_can_use_delta": False,
            "candidate_kind": "blocked",
            "block_reasons": ["reset_behavior", "activation_delta_unproven"],
        },
    )
    external = base_task(
        6,
        ["external"],
        node_kinds={"NODE_EXT_IN": 1},
        boundary={"has_state_update": False, "has_memory_write": False, "has_reset": False, "has_external": True, "has_special": False, "clock_names": []},
    )
    return {"format": "test.mt-23x-blocker-inspection.v1", "tasks": [local_dup, unknown_rhs, runtime_safe, memory_read, memory_write, reset_commit, external]}


def row_by_cpp(report, cpp_id):
    for row in report["top_blocked_candidates"]:
        if row["cpp_id"] == cpp_id:
            return row
    fail(f"missing blocked row for cpp_id {cpp_id}")


def main():
    repo = Path(__file__).resolve().parents[1]
    script = repo / "scripts" / "mt-23x-blocker-inspection-report.py"
    with tempfile.TemporaryDirectory(prefix="mt-23x-blockers-") as tmp:
        work = Path(tmp)
        schedule = work / "schedule.json"
        profile_log = work / "profile.log"
        json_out = work / "report.json"
        md_out = work / "report.md"
        schedule.write_text(json.dumps(fixture_schedule(), indent=2) + "\n")
        profile_log.write_text(profile({0: 30, 1: 25, 2: 20, 3: 15, 4: 14, 5: 13, 6: 12}))
        result = subprocess.run([
            sys.executable,
            str(script),
            "--schedule-json",
            str(schedule),
            "--profile-log",
            str(profile_log),
            "--json-out",
            str(json_out),
            "--md-out",
            str(md_out),
            "--top",
            "20",
        ], text=True, capture_output=True)
        if result.returncode != 0:
            fail(f"report command failed: {result.args}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        report = json.loads(json_out.read_text())

    expect(report["format"] == "gsim.mt-23x-blocker-inspection.v1", "unexpected report format")
    expect(report["totals"]["runtime_safe_expected_active_cost"] == 200, "runtime-safe cost should include only cpp_id 2")
    expect(report["totals"]["optimistic_runtime_safe_expected_active_cost"] == 0, "optimistic candidates must not be runtime-safe")
    expect(row_by_cpp(report, 0)["blocker_subreason"] == "state_update_local_safe_duplicate_writer_blocked", "duplicate writer subreason missing")
    expect(row_by_cpp(report, 1)["blocker_subreason"] == "state_update_unknown_rhs_timing", "unknown RHS subreason missing")
    expect(row_by_cpp(report, 3)["blocker_subreason"] == "memory_read_unsupported", "memory read subreason missing")
    expect(row_by_cpp(report, 4)["blocker_subreason"] == "memory_write_serial", "memory write subreason missing")
    expect(row_by_cpp(report, 5)["blocker_subreason"] == "reset_commit_or_side_effect", "reset commit subreason missing")
    expect(row_by_cpp(report, 6)["blocker_subreason"] == "external_or_blackbox", "external subreason missing")
    for row in report["top_blocked_candidates"]:
        expect(row.get("source_facts"), f"row {row['cpp_id']} missing source_facts")
        expect(row.get("must_remain_serial_reason"), f"row {row['cpp_id']} missing must_remain_serial_reason")
    print("mt-23x-blocker-inspection-report check ok")


if __name__ == "__main__":
    main()
