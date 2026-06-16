#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message):
    print(f"mt-22x-macro-fiber-planner check failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def task(
    cpp_id,
    static_cost,
    *,
    preds=None,
    succs=None,
    active_fanout=None,
    task_kind="pure_compute",
    node_kinds=None,
    serial_reasons=None,
    super_type="SUPER_VALID",
    boundary=None,
    state_update=None,
    state_update_group=None,
):
    if node_kinds is None:
        node_kinds = {"NODE_OTHERS": static_cost}
    boundary = boundary or {
        "has_state_update": False,
        "has_memory_write": False,
        "has_reset": False,
        "has_external": False,
        "has_special": False,
        "clock_names": [],
    }
    state_update = state_update or {"has_state_update": False}
    state_update_group = state_update_group or {
        "local_safe_candidate": False,
        "runtime_safe_candidate": False,
        "runtime_block_reasons": [],
    }
    return {
        "cpp_id": cpp_id,
        "scan_index": cpp_id,
        "super_id": 1000 + cpp_id,
        "super_type": super_type,
        "task_kind": task_kind,
        "serial_reasons": serial_reasons or ([] if task_kind == "pure_compute" else ["serial"]),
        "active_word": cpp_id // 8,
        "active_mask": hex(1 << (cpp_id % 8)),
        "node_kinds": node_kinds,
        "pred_cpp_ids": preds or [],
        "succ_cpp_ids": succs or [],
        "active_fanout": active_fanout or [],
        "boundary": boundary,
        "state_update": state_update,
        "state_update_group": state_update_group,
        "repcut": {"candidate_cost": static_cost},
    }


def profile_log(counts, cycles=100):
    task_counts = ",".join(f"{cpp_id}:{count}" for cpp_id, count in sorted(counts.items()) if count)
    return (
        "[mt-profile] helper_mode=mt worker_count=4 worker_pool=1 min_batch_tasks=1 "
        f"max_worker_count=4 cycles={cycles} active_word_count=1 serial_tasks=0 pure_tasks=0 "
        "pure_batch_count=0 true_parallel_batch_count=0 skipped_fake_parallel_batch_count=0 "
        "serial_fast_task_count=0 batch_wall_ns=0 true_parallel_wall_ns=0 serial_wall_ns=0 "
        "merge_wall_ns=0 total_step_ns=0\n"
        f"[mt-profile] task_cpp_ids={task_counts}\n"
    )


def run_planner(work_dir, name, schedule, counts, *extra_args):
    repo = Path(__file__).resolve().parents[1]
    planner = repo / "scripts" / "mt-22x-macro-fiber-repcut-planner.py"
    schedule_path = work_dir / f"{name}_schedule.json"
    profile_path = work_dir / f"{name}_profile.log"
    json_path = work_dir / f"{name}_report.json"
    md_path = work_dir / f"{name}_report.md"
    schedule_path.write_text(json.dumps({"format": "test.schedule.v1", "tasks": schedule}, indent=2) + "\n")
    profile_path.write_text(profile_log(counts))
    command = [
        sys.executable,
        str(planner),
        "--schedule-json",
        str(schedule_path),
        "--profile-log",
        str(profile_path),
        "--json-out",
        str(json_path),
        "--md-out",
        str(md_path),
        "--target-partitions",
        "2,4",
        *extra_args,
    ]
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode != 0:
        fail(
            f"planner failed for {name}: {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    expect(json_path.is_file(), f"planner did not write {json_path}")
    expect(md_path.is_file(), f"planner did not write {md_path}")
    return json.loads(json_path.read_text())


def report_for(result, target):
    reports = {
        int(row["target_partitions"]): row
        for row in result.get("partition_reports", [])
    }
    expect(target in reports, f"missing target partition report t{target}")
    return reports[target]


def check_shared_replication(work_dir):
    schedule = [
        task(0, 1, succs=[1, 2], active_fanout=[1, 2]),
        task(1, 2, preds=[0], node_kinds={"NODE_OTHERS": 1, "NODE_OUT": 1}),
        task(2, 2, preds=[0], node_kinds={"NODE_OTHERS": 1, "NODE_OUT": 1}),
    ]
    result = run_planner(
        work_dir,
        "shared_replication",
        schedule,
        {0: 20, 1: 20, 2: 20},
        "--max-duplicate-expected-cost",
        "1000",
        "--replication-budget-ratio",
        "2.0",
    )
    t2 = report_for(result, 2)
    expect(t2["unique_expected_cost"] == 100, f"unexpected unique expected cost: {t2}")
    expect(t2["duplicated_cpp_id_count"] >= 1, f"expected at least one duplicated cppId: {t2}")
    expect(any(row["cpp_id"] == 0 for row in t2["top_replicated_nodes"]), f"shared producer was not replicated: {t2}")
    expect(t2["estimated_barrier_reduction"] > 0, f"expected positive barrier reduction: {t2}")


def check_budget_rejection(work_dir):
    schedule = [
        task(0, 100, succs=[1, 2], active_fanout=[1, 2]),
        task(1, 1, preds=[0], node_kinds={"NODE_OUT": 1}),
        task(2, 1, preds=[0], node_kinds={"NODE_OUT": 1}),
    ]
    result = run_planner(
        work_dir,
        "budget_rejection",
        schedule,
        {0: 10, 1: 10, 2: 10},
        "--max-duplicate-expected-cost",
        "50",
        "--replication-budget-ratio",
        "0.05",
    )
    t2 = report_for(result, 2)
    expect(t2["duplicated_cpp_id_count"] == 0, f"high-copy producer should not be duplicated: {t2}")
    reasons = {row["reason"] for row in t2["top_blockers"]}
    expect("replication_budget" in reasons, f"missing replication_budget blocker: {t2['top_blockers']}")


def check_blocker_classification(work_dir):
    memory_boundary = {
        "has_state_update": False,
        "has_memory_write": True,
        "has_reset": False,
        "has_external": False,
        "has_special": False,
        "clock_names": [],
    }
    schedule = [
        task(
            0,
            5,
            succs=[1],
            task_kind="serial",
            node_kinds={"NODE_MEM": 1},
            serial_reasons=["memory_write"],
            boundary=memory_boundary,
        ),
        task(1, 1, preds=[0], node_kinds={"NODE_OUT": 1}),
    ]
    result = run_planner(work_dir, "blocker_classification", schedule, {0: 8, 1: 8})
    candidate = next(row for row in result["candidates"] if row["root_cpp_id"] == 1)
    blockers = {row["reason"] for row in candidate["blocked_units"]}
    expect("memory" in blockers, f"memory predecessor was not classified as memory blocker: {candidate}")


def check_activity_weighting(work_dir):
    schedule = [
        task(0, 100, node_kinds={"NODE_OUT": 100}),
        task(1, 60, node_kinds={"NODE_OUT": 60}),
        task(2, 50, node_kinds={"NODE_OUT": 50}),
    ]
    result = run_planner(work_dir, "activity_weighting", schedule, {0: 1, 1: 100, 2: 100})
    t2 = report_for(result, 2)
    active_ratio = t2["active_weighted_assignment"]["expected_imbalance_ratio"]
    static_ratio = t2["static_only_assignment"]["expected_imbalance_ratio"]
    expect(active_ratio < static_ratio, f"activity-weighted balance did not beat static-only: {t2}")


def main():
    with tempfile.TemporaryDirectory(prefix="mt-22x-planner-") as tmp:
        work_dir = Path(tmp)
        check_shared_replication(work_dir)
        check_budget_rejection(work_dir)
        check_blocker_classification(work_dir)
        check_activity_weighting(work_dir)
    print("mt-22x-macro-fiber-planner check ok")


if __name__ == "__main__":
    main()
