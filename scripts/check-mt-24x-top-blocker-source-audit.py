#!/usr/bin/env python3
"""Focused fixture gate for mt-24x-top-blocker-source-audit.py.

Uses synthetic schedules so the gate is fast and hermetic.  The fixture
validates that the auditor:

- clusters top blocked rows by source/codegen shape;
- sub-classifies memory_dynamic_or_array_index and state_update_unknown_rhs_timing;
- assigns triage results and recoverable cost estimates;
- produces a 24X-B decision using the 20%-30% threshold.
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message):
    print(f"mt-24x-top-blocker-source-audit check failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def task(
    cpp_id,
    *,
    serial_reasons,
    node_kinds,
    state_target_names=None,
    state_target_count=0,
    rhs_timing_class="unknown",
    rhs_timing_evidence="multiple_state_targets",
    rhs_reads_same_cycle_target=False,
    has_memory_or_dynamic_array=False,
    has_reset_behavior=False,
    has_state_update=True,
    boundary=None,
    group=None,
    preds=None,
    succs=None,
):
    boundary = boundary or {
        "has_state_update": has_state_update,
        "has_memory_write": "memory_write" in serial_reasons,
        "has_reset": has_reset_behavior or "reset" in serial_reasons,
        "has_external": False,
        "has_special": False,
        "clock_names": ["clock"] if has_state_update else [],
    }
    state_update = {
        "has_state_update": has_state_update,
        "state_target_names": state_target_names or [],
        "state_target_count": state_target_count,
        "single_target": state_target_count == 1,
        "rhs_timing_class": rhs_timing_class,
        "rhs_timing_evidence": rhs_timing_evidence,
        "rhs_reads_state_targets": [],
        "rhs_reads_same_cycle_target": rhs_reads_same_cycle_target,
        "has_reset_behavior": has_reset_behavior,
        "has_async_reset_behavior": False,
        "has_memory_or_dynamic_array": has_memory_or_dynamic_array,
        "has_external_or_special": False,
        "activation_fanout_count": 0,
        "activation_can_use_delta": False,
        "candidate_kind": "blocked" if rhs_timing_class == "unknown" else "safe_candidate",
        "block_reasons": ["rhs_timing_unknown"] if rhs_timing_class == "unknown" else [],
    }
    group = group or {
        "local_safe_candidate": False,
        "runtime_safe_candidate": False,
        "target_writer_count": 1,
        "target_multi_target_writer_count": 0,
        "target_writer_universe_complete": True,
        "target_writer_cpp_ids": [cpp_id],
        "target_writer_conflict_kind": "unique_writer",
        "target_writer_proof": "target_unique_writer",
        "runtime_block_reasons": ["not_local_safe_candidate"],
    }
    return {
        "cpp_id": cpp_id,
        "scan_index": cpp_id,
        "super_id": 1000 + cpp_id,
        "super_type": "SUPER_VALID",
        "task_kind": "serial",
        "serial_reasons": serial_reasons,
        "active_word": cpp_id // 8,
        "active_mask": hex(1 << (cpp_id % 8)),
        "node_kinds": node_kinds,
        "pred_cpp_ids": preds or [],
        "succ_cpp_ids": succs or [],
        "active_fanout": [],
        "boundary": boundary,
        "state_update": state_update,
        "state_update_group": group,
        "repcut": {"candidate_cost": max(1, sum(node_kinds.values()))},
    }


def profile_log(counts, cycles=100):
    body = ",".join(f"{cpp_id}:{count}" for cpp_id, count in sorted(counts.items()) if count)
    return (
        "[mt-profile] helper_mode=mt worker_count=4 worker_pool=1 min_batch_tasks=1 "
        f"max_worker_count=4 cycles={cycles} active_word_count=1 serial_tasks=0 pure_tasks=0 "
        "pure_batch_count=0 true_parallel_batch_count=0 skipped_fake_parallel_batch_count=0 "
        "serial_fast_task_count=0 batch_wall_ns=0 true_parallel_wall_ns=0 serial_wall_ns=0 "
        "merge_wall_ns=0 total_step_ns=0\n"
        f"[mt-profile] task_cpp_ids={body}\n"
    )


def blocker_row(cpp_id, subreason, static_cost, active_count, source_facts):
    return {
        "cpp_id": cpp_id,
        "super_id": 1000 + cpp_id,
        "task_kind": "serial",
        "super_type": "SUPER_VALID",
        "blocker_reason": "memory" if subreason.startswith("memory") else "state_update_proof",
        "blocker_subreason": subreason,
        "static_cost": static_cost,
        "active_count": active_count,
        "expected_active_cost": static_cost * active_count,
        "safety_kind": "optimistic-only",
        "source_facts": source_facts,
        "missing_proof_field": "test",
        "must_remain_serial_reason": "test",
    }


def build_fixture():
    # A small schedule with two memory dynamic-index tasks and two state-update
    # unknown-RHS tasks, plus an unrelated filler so the total is non-trivial.
    schedule_tasks = [
        task(
            0,
            serial_reasons=["state_update", "array_or_dynamic_index"],
            node_kinds={"NODE_OTHERS": 100, "NODE_REG_DST": 10},
            state_target_names=["modA__DOT__r0"],
            state_target_count=1,
            rhs_timing_class="unknown",
            rhs_timing_evidence="multiple_state_targets",
            rhs_reads_same_cycle_target=False,
            has_memory_or_dynamic_array=True,
        ),
        task(
            1,
            serial_reasons=["array_or_dynamic_index"],
            node_kinds={"NODE_OTHERS": 80},
            has_state_update=False,
            has_memory_or_dynamic_array=True,
        ),
        task(
            2,
            serial_reasons=["state_update"],
            node_kinds={"NODE_OTHERS": 50, "NODE_REG_DST": 5},
            state_target_names=["modB__DOT__q0", "modB__DOT__q1"],
            state_target_count=2,
            rhs_timing_class="unknown",
            rhs_timing_evidence="multiple_state_targets",
            rhs_reads_same_cycle_target=True,
        ),
        task(
            3,
            serial_reasons=["state_update"],
            node_kinds={"NODE_OTHERS": 40, "NODE_REG_DST": 2},
            state_target_names=["modC__DOT__s0"],
            state_target_count=1,
            rhs_timing_class="unknown",
            rhs_timing_evidence="single_target_old_state",
            rhs_reads_same_cycle_target=False,
        ),
    ]
    counts = {0: 10, 1: 10, 2: 10, 3: 10}

    inspection = {
        "format": "gsim.mt-23x-blocker-inspection.v1",
        "inputs": {"schedule_json": "test", "profile_log": "test", "source_commit": "test"},
        "profile": {"cycles": 100, "main": {"cycles": "100"}},
        "totals": {
            "task_count": 5,
            "active_task_count": 5,
            # Inflate total so the four audited blockers are well below the 20% threshold.
            "total_expected_active_cost": 20000,
            "blocked_expected_active_cost": 0,
            "runtime_safe_expected_active_cost": 0,
            "optimistic_runtime_safe_expected_active_cost": 0,
        },
        "cost_by_blocker_reason": {},
        "cost_by_blocker_subreason": {},
        "count_by_blocker_subreason": {},
        "runtime_safe_candidates": [],
        "top_blocked_candidates": [
            blocker_row(
                0,
                "memory_dynamic_or_array_index",
                110,
                10,
                {
                    "serial_reasons": ["state_update", "array_or_dynamic_index"],
                    "node_kinds": {"NODE_OTHERS": 100, "NODE_REG_DST": 10},
                    "boundary": schedule_tasks[0]["boundary"],
                    "state_update": schedule_tasks[0]["state_update"],
                    "state_update_group": schedule_tasks[0]["state_update_group"],
                },
            ),
            blocker_row(
                1,
                "memory_dynamic_or_array_index",
                80,
                10,
                {
                    "serial_reasons": ["array_or_dynamic_index"],
                    "node_kinds": {"NODE_OTHERS": 80},
                    "boundary": schedule_tasks[1]["boundary"],
                    "state_update": schedule_tasks[1]["state_update"],
                    "state_update_group": schedule_tasks[1]["state_update_group"],
                },
            ),
            blocker_row(
                2,
                "state_update_unknown_rhs_timing",
                55,
                10,
                {
                    "serial_reasons": ["state_update"],
                    "node_kinds": {"NODE_OTHERS": 50, "NODE_REG_DST": 5},
                    "boundary": schedule_tasks[2]["boundary"],
                    "state_update": schedule_tasks[2]["state_update"],
                    "state_update_group": schedule_tasks[2]["state_update_group"],
                },
            ),
            blocker_row(
                3,
                "state_update_unknown_rhs_timing",
                42,
                10,
                {
                    "serial_reasons": ["state_update"],
                    "node_kinds": {"NODE_OTHERS": 40, "NODE_REG_DST": 2},
                    "boundary": schedule_tasks[3]["boundary"],
                    "state_update": schedule_tasks[3]["state_update"],
                    "state_update_group": schedule_tasks[3]["state_update_group"],
                },
            ),
        ],
    }
    return schedule_tasks, counts, inspection


def run_audit(work_dir, schedule, counts, inspection):
    repo = Path(__file__).resolve().parents[1]
    auditor = repo / "scripts" / "mt-24x-top-blocker-source-audit.py"
    schedule_path = work_dir / "schedule.json"
    inspection_path = work_dir / "inspection.json"
    json_out = work_dir / "audit.json"
    md_out = work_dir / "audit.md"

    schedule_path.write_text(json.dumps({"format": "gsim.mt-schedule.v1", "tasks": schedule}, indent=2) + "\n")
    inspection_path.write_text(json.dumps(inspection, indent=2) + "\n")

    result = subprocess.run(
        [
            sys.executable,
            str(auditor),
            "--blocker-inspection-json",
            str(inspection_path),
            "--schedule-json",
            str(schedule_path),
            "--json-out",
            str(json_out),
            "--md-out",
            str(md_out),
            "--top",
            "10",
        ],
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        fail(f"auditor failed:\n{result.stdout}\n{result.stderr}")
    return json.loads(json_out.read_text())


def family_by_subreason(report, subreason):
    for family in report["families"]:
        if family["subreason"] == subreason:
            return family
    fail(f"missing family {subreason}")


def main():
    repo = Path(__file__).resolve().parents[1]
    auditor = repo / "scripts" / "mt-24x-top-blocker-source-audit.py"
    expect(auditor.is_file(), f"auditor script missing: {auditor}")

    with tempfile.TemporaryDirectory(prefix="mt-24x-audit-") as tmp:
        work_dir = Path(tmp)
        schedule, counts, inspection = build_fixture()
        report = run_audit(work_dir, schedule, counts, inspection)

    expect(report["format"] == "gsim.mt-24x-top-blocker-source-audit.v1", "unexpected audit format")

    total = report["totals"]["total_expected_active_cost"]
    expect(total > 0, "total expected active cost must be positive")

    memory_family = family_by_subreason(report, "memory_dynamic_or_array_index")
    state_family = family_by_subreason(report, "state_update_unknown_rhs_timing")

    # cpp_id 0 is array_or_mux_compute (state update + dynamic array, no mem write).
    # cpp_id 1 is read_only_dynamic_index (no state update, no mem write).
    memory_subclasses = set(memory_family["sub_class_distribution"].keys())
    expect("array_or_mux_compute" in memory_subclasses, "expected array_or_mux_compute memory sub-class")
    expect("read_only_dynamic_index" in memory_subclasses, "expected read_only_dynamic_index memory sub-class")

    # cpp_id 2 is rhs_compute_can_be_precomputed (same-cycle read, multi target).
    # cpp_id 3 is old_state_only_candidate (old_state evidence, no same-cycle read).
    state_subclasses = set(state_family["sub_class_distribution"].keys())
    expect("rhs_compute_can_be_precomputed" in state_subclasses, "expected rhs_compute_can_be_precomputed state sub-class")
    expect("old_state_only_candidate" in state_subclasses, "expected old_state_only_candidate state sub-class")

    # Recoverable cost should be positive but below the 20% threshold -> pivot.
    decision = report["decision"]
    expect(decision["decision"] == "pivot", f"expected pivot decision for low recoverable share: {decision}")
    expect(decision["threshold_range"] == [0.2, 0.3], "unexpected threshold range")
    expect(report["totals"]["recoverable_expected_active_cost"] > 0, "recoverable cost must be positive")

    print("mt-24x-top-blocker-source-audit check ok")


if __name__ == "__main__":
    main()
