#!/usr/bin/env python3
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message):
    print(f"mt-15x-state-update-metadata check failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def run(command, timeout):
    result = subprocess.run(command, text=True, capture_output=True, timeout=timeout)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        fail(f"command failed ({result.returncode}): {' '.join(str(part) for part in command)}")
    return result


def schedule_path_for(model_dir, fir_path):
    return model_dir / f"{fir_path.stem}_mt_schedule.json"


def generate_schedule(gsim_bin, fir_path, model_dir, timeout):
    return generate_schedule_with_flags(gsim_bin, fir_path, model_dir, timeout, ["--supernode-max-size=1"])


def generate_schedule_with_flags(gsim_bin, fir_path, model_dir, timeout, extra_flags):
    model_dir.mkdir(parents=True, exist_ok=True)
    run([
        str(gsim_bin),
        "--dir",
        str(model_dir),
        *extra_flags,
        "--mt-helper-mode=mt",
        "--dump-mt-schedule-json",
        str(fir_path),
    ], timeout)
    schedule_path = schedule_path_for(model_dir, fir_path)
    expect(schedule_path.is_file(), f"missing schedule JSON {schedule_path}")
    return json.loads(schedule_path.read_text()), schedule_path


def state_update_tasks(schedule):
    tasks = schedule.get("tasks") or []
    return [task for task in tasks if (task.get("state_update") or {}).get("has_state_update")]


def target_tasks(schedule, target):
    return [
        task for task in state_update_tasks(schedule)
        if target in ((task.get("state_update") or {}).get("state_target_names") or [])
    ]


def check_common_schedule(schedule):
    expect(schedule.get("format") == "gsim.mt-schedule.v1", "unexpected schedule format")
    tasks = schedule.get("tasks")
    expect(isinstance(tasks, list) and tasks, "schedule has no tasks")
    for task in tasks:
        su = task.get("state_update")
        expect(isinstance(su, dict), f"task {task.get('cpp_id')} missing state_update object")
        expect(su.get("has_state_update") == (task.get("boundary") or {}).get("has_state_update"),
               f"task {task.get('cpp_id')} state_update/boundary mismatch")
        expect(su.get("state_target_count") == len(su.get("state_target_names") or []),
               f"task {task.get('cpp_id')} target count mismatch")
        expect(su.get("single_target") == (su.get("state_target_count") == 1),
               f"task {task.get('cpp_id')} single_target mismatch")
        if su.get("rhs_timing_class") == "unknown":
            expect(su.get("candidate_kind") == "blocked",
                   f"task {task.get('cpp_id')} unknown RHS timing must stay blocked")
        if su.get("rhs_reads_same_cycle_target"):
            expect(su.get("candidate_kind") == "blocked",
                   f"task {task.get('cpp_id')} same-cycle target read must stay blocked")
            expect("rhs_reads_same_cycle_target" in su.get("block_reasons", []),
                   f"task {task.get('cpp_id')} same-cycle target read must record block reason")
        group = task.get("state_update_group")
        expect(isinstance(group, dict), f"task {task.get('cpp_id')} missing state_update_group object")
        expect(group.get("local_safe_candidate") == (su.get("candidate_kind") == "safe_candidate"),
               f"task {task.get('cpp_id')} local_safe_candidate must mirror candidate_kind")
        expect(isinstance(group.get("target_multi_target_writer_count"), int),
               f"task {task.get('cpp_id')} missing target_multi_target_writer_count")
        expect(isinstance(group.get("target_writer_universe_complete"), bool),
               f"task {task.get('cpp_id')} missing target_writer_universe_complete")
        if group.get("target_writer_count", 0) > 1:
            expect(group.get("target_writer_conflict_kind") == "multi_writer_unproven",
                   f"task {task.get('cpp_id')} duplicate writers must be unproven")
            expect(not group.get("runtime_safe_candidate"),
                   f"task {task.get('cpp_id')} duplicate writers must block runtime-safe")
            expect("target_multi_writer_unproven" in group.get("runtime_block_reasons", []),
                   f"task {task.get('cpp_id')} duplicate writers must record runtime block reason")
        if group.get("runtime_safe_candidate"):
            expect(group.get("local_safe_candidate"),
                   f"task {task.get('cpp_id')} runtime-safe must be local-safe")
            expect(group.get("target_writer_conflict_kind") == "unique_writer",
                   f"task {task.get('cpp_id')} runtime-safe must be unique writer")
            expect(not group.get("runtime_block_reasons"),
                   f"task {task.get('cpp_id')} runtime-safe must not carry runtime block reasons")


def check_state_fixture(schedule):
    check_common_schedule(schedule)
    for target in ("r0", "r1", "rr", "chain_a", "chain_b", "d_only", "unique_only", "dup_writer"):
        rows = target_tasks(schedule, target)
        expect(rows, f"missing stable state target name {target}")
        for task in rows:
            su = task["state_update"]
            expect(su["state_target_count"] == 1, f"{target} task should be single-target")
            expect(su["candidate_kind"] in {"safe_candidate", "blocked"}, f"{target} task has invalid candidate kind")

    precomputed_targets = {
        target
        for task in state_update_tasks(schedule)
        for target in task["state_update"].get("state_target_names", [])
        if task["state_update"].get("rhs_timing_class") == "precomputed"
    }
    old_state_only_targets = {
        target
        for task in state_update_tasks(schedule)
        for target in task["state_update"].get("state_target_names", [])
        if task["state_update"].get("rhs_timing_class") == "old_state_only"
    }
    expect({"r0", "r1", "chain_a", "chain_b", "d_only"}.intersection(precomputed_targets),
           "simple register commits must expose at least one precomputed RHS timing proof")
    expect("d_only" in old_state_only_targets,
           "input-only D register must expose old_state_only RHS timing proof")

    safe_targets = {
        target
        for task in state_update_tasks(schedule)
        for target in task["state_update"].get("state_target_names", [])
        if task["state_update"].get("candidate_kind") == "safe_candidate"
    }
    expect("d_only" in safe_targets,
           "input-only D register should be a safe metadata candidate")
    expect("unique_only" in safe_targets,
           "unique single-writer register should be a local-safe metadata candidate")
    expect({"r0", "r1", "chain_a"}.intersection(old_state_only_targets),
           "simple next-state updates must expose at least one old_state_only RHS timing proof")

    runtime_safe_targets = {
        target
        for task in state_update_tasks(schedule)
        for target in task["state_update"].get("state_target_names", [])
        if (task.get("state_update_group") or {}).get("runtime_safe_candidate")
    }
    expect("unique_only" not in runtime_safe_targets,
           "register update/commit pair should not become runtime-safe without pair-aware writer proof")
    expect(not runtime_safe_targets or all(
        (task.get("state_update_group") or {}).get("target_writer_conflict_kind") == "unique_writer"
        for task in state_update_tasks(schedule)
        if (task.get("state_update_group") or {}).get("runtime_safe_candidate")
    ), "any runtime-safe target must have unique-writer proof")

    dup_rows = target_tasks(schedule, "dup_writer")
    expect(any((task.get("state_update_group") or {}).get("local_safe_candidate") for task in dup_rows),
           "duplicate-writer fixture must expose at least one local-safe writer")
    expect(any((task.get("state_update_group") or {}).get("target_writer_count", 0) > 1 for task in dup_rows),
           "duplicate-writer fixture must record multiple writers")
    expect(not any((task.get("state_update_group") or {}).get("runtime_safe_candidate") for task in dup_rows),
           "duplicate-writer fixture must not become runtime-safe without mutual-exclusion proof")
    expect(any("target_multi_writer_unproven" in (task.get("state_update_group") or {}).get("runtime_block_reasons", [])
               for task in dup_rows),
           "duplicate-writer fixture must record target_multi_writer_unproven")

    chain_b_rows = target_tasks(schedule, "chain_b")
    expect(any(task["state_update"].get("rhs_reads_same_cycle_target") for task in chain_b_rows),
           "register chain must record same-cycle read hazard")
    expect(not any(task["state_update"].get("candidate_kind") == "safe_candidate" and
                   task["state_update"].get("rhs_reads_same_cycle_target")
                   for task in chain_b_rows),
           "register chain hazard task must not become a safe metadata candidate")

    reset_rows = target_tasks(schedule, "rr")
    expect(any((task["state_update"]).get("has_reset_behavior") for task in reset_rows),
           "reset-sensitive register rr must carry reset behavior metadata")
    expect(any("reset_behavior" in task["state_update"].get("block_reasons", []) for task in reset_rows),
           "reset-sensitive register rr must be blocked by reset_behavior")
    expect(any(task["state_update"].get("activation_fanout_count", 0) > 0 for task in state_update_tasks(schedule)),
           "state-update fixture must expose activation fanout metadata")


def check_memory_fixture(schedule):
    check_common_schedule(schedule)
    memory_blocked = [
        task for task in schedule.get("tasks", [])
        if (task.get("state_update") or {}).get("has_memory_or_dynamic_array")
        and (task.get("state_update") or {}).get("candidate_kind") == "blocked"
    ]
    expect(memory_blocked, "memory fixture must expose blocked memory/dynamic-array metadata")
    expect(any("memory_or_dynamic_array" in (task.get("state_update") or {}).get("block_reasons", [])
               for task in memory_blocked),
           "memory fixture must add memory_or_dynamic_array block reason")


def check_writer_universe_fixture(schedule):
    check_common_schedule(schedule)
    by_target = {}
    for task in state_update_tasks(schedule):
        for target in (task.get("state_update") or {}).get("state_target_names", []):
            by_target.setdefault(target, []).append(task)

    overlap_target = None
    overlap_single_local_safe = []
    for target, rows in sorted(by_target.items()):
        single_local_safe = [
            task for task in rows
            if (task.get("state_update") or {}).get("state_target_count") == 1
            and (task.get("state_update_group") or {}).get("local_safe_candidate")
        ]
        multi_target_rows = [
            task for task in rows
            if (task.get("state_update") or {}).get("state_target_count", 0) > 1
        ]
        if single_local_safe and multi_target_rows:
            overlap_target = target
            overlap_single_local_safe = single_local_safe
            break

    expect(overlap_target is not None,
           "writer-universe fixture must include a local-safe single-target writer sharing a target with a multi-target writer")
    for task in overlap_single_local_safe:
        group = task.get("state_update_group") or {}
        expect(group.get("target_writer_count", 0) >= 2,
               f"{overlap_target} single-target writer must count the multi-target writer in target_writer_count")
        expect(group.get("target_multi_target_writer_count", 0) >= 1,
               f"{overlap_target} single-target writer must count overlapping multi-target writers")
        expect(group.get("target_writer_conflict_kind") == "multi_writer_unproven",
               f"{overlap_target} single-target writer must not claim unique_writer")
        expect(not group.get("runtime_safe_candidate"),
               f"{overlap_target} single-target writer must not become runtime-safe")
        expect("target_multi_writer_unproven" in group.get("runtime_block_reasons", []),
               f"{overlap_target} single-target writer must record target_multi_writer_unproven")
        expect("target_multi_target_writer_unproven" in group.get("runtime_block_reasons", []),
               f"{overlap_target} single-target writer must record target_multi_target_writer_unproven")


def main():
    if len(sys.argv) != 4:
        fail("usage: check-mt-15x-state-update-metadata.py <gsim-bin> <test-dir> <work-dir>")

    gsim_bin = Path(sys.argv[1])
    test_dir = Path(sys.argv[2])
    work_dir = Path(sys.argv[3])
    expect(gsim_bin.is_file(), f"{gsim_bin} does not exist")
    expect(test_dir.is_dir(), f"{test_dir} does not exist")
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True)

    timeout = 30
    state_schedule, state_path = generate_schedule(
        gsim_bin,
        test_dir / "mt-state-update-proof-gate.fir",
        work_dir / "state",
        timeout,
    )
    memory_schedule, memory_path = generate_schedule(
        gsim_bin,
        test_dir / "mt-state-update-memory-proof-gate.fir",
        work_dir / "memory",
        timeout,
    )
    writer_universe_schedule, writer_universe_path = generate_schedule_with_flags(
        gsim_bin,
        test_dir / "mt-state-update-writer-universe-proof-gate.fir",
        work_dir / "writer-universe",
        timeout,
        ["--supernode-max-size=4"],
    )

    checker = Path(__file__).with_name("check-mt-schedule-json.py")
    run([sys.executable, str(checker), str(state_path)], timeout)
    run([sys.executable, str(checker), str(memory_path)], timeout)
    run([sys.executable, str(checker), str(writer_universe_path)], timeout)
    check_state_fixture(state_schedule)
    check_memory_fixture(memory_schedule)
    check_writer_universe_fixture(writer_universe_schedule)

    with tempfile.TemporaryDirectory(prefix="gsim-mt-15x-orword-") as tmp:
        orword = Path(__file__).with_name("check-mt-activation-delta-orword.py")
        run([sys.executable, str(orword), str(work_dir / "state")], timeout)

    print(
        "mt-15x state-update metadata ok: "
        f"{len(state_update_tasks(state_schedule))} state fixture state-update tasks, "
        f"{len(memory_schedule.get('tasks', []))} memory fixture tasks, "
        f"{len(state_update_tasks(writer_universe_schedule))} writer-universe fixture state-update tasks"
    )


if __name__ == "__main__":
    main()
