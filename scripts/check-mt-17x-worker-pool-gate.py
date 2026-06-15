#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def fail(message):
    print(f"mt-17x-worker-pool-gate failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def run(command, *, env=None, timeout=None):
    try:
        result = subprocess.run(command, text=True, capture_output=True, env=env, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        fail(f"command timed out: {' '.join(command)}\nstdout:\n{exc.stdout or ''}\nstderr:\n{exc.stderr or ''}")
    if result.returncode != 0:
        fail(f"command failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def model_info(model_dir):
    headers = sorted(model_dir.glob("*.h"))
    expect(len(headers) == 1, f"expected one generated header under {model_dir}, found {len(headers)}")
    header = headers[0]
    text = header.read_text()
    class_match = re.search(r"\bclass\s+S([A-Za-z_][A-Za-z0-9_]*)\s*\{", text)
    expect(class_match is not None, f"could not find generated model class in {header}")
    top = class_match.group(1)
    setters = re.findall(r"\bvoid\s+set_([A-Za-z_][A-Za-z0-9_]*)\(([^)]+?)\s+val\);", text)
    getters = re.findall(r"\b([A-Za-z0-9_:_BitInt<>\s]+?)\s+get_([A-Za-z_][A-Za-z0-9_]*)\(\);", text)
    expect(setters, f"no generated input setters found in {header}")
    expect(getters, f"no generated output getters found in {header}")
    sources = sorted(model_dir.glob("*.cpp"))
    expect(sources, f"no generated C++ sources found under {model_dir}")
    return top, setters, getters, header, sources


def driver_source(top, setters, getters, header_name):
    lines = [
        "#include <cctype>",
        "#include <cstdint>",
        "#include <iostream>",
        "#include <string>",
        f"#include \"{header_name}\"",
        "",
        "static uint64_t driveValue(const char *name, int cycle, int index) {",
        "  std::string n(name);",
        "  std::string lower;",
        "  for (char c : n) lower += (char)std::tolower((unsigned char)c);",
        "  if (lower.find(\"clock\") != std::string::npos) return cycle & 1;",
        "  if (lower.find(\"reset\") != std::string::npos) return 0;",
        "  if (lower.find(\"io_g\") != std::string::npos) return (cycle + index) & 1;",
        "  return (uint64_t)((cycle + 3) * (index + 5) + (cycle & 7));",
        "}",
        "",
        "int main() {",
        f"  S{top} dut;",
        "  for (int cycle = 0; cycle < 24; cycle ++) {",
    ]
    for index, (name, typ) in enumerate(setters):
        lines.append(f"    dut.set_{name}(({typ})driveValue(\"{name}\", cycle, {index}));")
    lines.append("    dut.step();")
    lines.append("    std::cout << cycle;")
    for _, name in getters:
        lines.append(f"    std::cout << ' ' << (unsigned long long)dut.get_{name}();")
    lines.extend([
        "    std::cout << '\\n';",
        "  }",
        "  return 0;",
        "}",
    ])
    return "\n".join(lines) + "\n"


def build_driver(model_dir, work_dir, label):
    top, setters, getters, header, sources = model_info(model_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    driver = work_dir / f"{label}_driver.cpp"
    binary = work_dir / f"{label}_driver"
    driver.write_text(driver_source(top, setters, getters, header.name))
    compiler = os.environ.get("CXX", "clang++")
    run([
        compiler,
        "-std=c++17",
        "-O0",
        "-pthread",
        "-I",
        str(model_dir),
        str(driver),
        *[str(source) for source in sources],
        "-o",
        str(binary),
    ])
    return binary


def parse_key_values(line):
    return {key: value.rstrip(",") for key, value in re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", line)}


def profile_line(stderr, prefix):
    for line in stderr.splitlines():
        if line.startswith(prefix):
            return line
    fail(f"missing profile line {prefix}")


def profile_int(values, key):
    expect(key in values, f"missing profile key {key}")
    return int(values[key].replace(",", ""))


def generated_text(model_dir):
    paths = sorted(model_dir.glob("*.h")) + sorted(model_dir.glob("*.cpp"))
    return "\n".join(path.read_text() for path in paths)


def check_structure(model_dir):
    text = generated_text(model_dir)
    for token in (
        "mtWorkerPoolEnabled",
        "GSIM_MT_WORKER_POOL",
        "mtWorkerPoolLoop",
        "startMtWorkerPool",
        "stopMtWorkerPool",
        "std::condition_variable",
        "mtWorkerPoolGeneration",
        "mtRunPureBatchWorkerRange",
    ):
        expect(token in text, f"generated MT model missing worker-pool token {token}")
    expect("std::vector<std::thread> workers;" in text,
           "generated MT model no longer exposes per-batch std::thread A/B path")
    expect("worker_pool=%d" in text, "profile output does not record worker_pool")
    expect("ActivationDelta" in text, "generated MT model has no ActivationDelta")
    expect(re.search(r"mtRunPureBatch\(\d+,\s*\d+,\s*oldFlag\);", text),
           "generated MT model has no pure batch dispatch")


def run_driver(binary, env_overrides):
    env = os.environ.copy()
    env.update(env_overrides)
    return run([str(binary)], env=env)


def verify_run(label, result, seq_stdout, expected_pool, expect_parallel):
    expect(seq_stdout == result.stdout, f"{label} trace differs from seq trace")
    main_values = parse_key_values(profile_line(result.stderr, "[mt-profile] helper_mode="))
    delta_values = parse_key_values(profile_line(result.stderr, "[mt-profile] activation_delta"))
    worker_line = profile_line(result.stderr, "[mt-profile] worker_task_count=")
    expect(profile_int(main_values, "worker_pool") == expected_pool,
           f"{label} profile worker_pool mismatch")
    if expect_parallel:
        expect(profile_int(main_values, "true_parallel_batch_count") > 0,
               f"{label} did not enter true-parallel batches")
        expect(profile_int(main_values, "max_worker_count") > 1,
               f"{label} did not use more than one worker")
        expect(profile_int(delta_values, "entries") > 0,
               f"{label} did not record ActivationDelta entries")
        counts = [int(value) for value in worker_line.split("=", 1)[1].split(",") if value]
        expect(sum(1 for value in counts if value > 0) > 1,
               f"{label} worker_task_count shows only one active worker: {counts}")
    return main_values, delta_values, worker_line


def stable_profile(stderr):
    main_values = parse_key_values(profile_line(stderr, "[mt-profile] helper_mode="))
    for key in (
        "batch_wall_ns",
        "true_parallel_wall_ns",
        "serial_wall_ns",
        "merge_wall_ns",
        "total_step_ns",
    ):
        main_values.pop(key, None)
    return {
        "main": main_values,
        "activation_delta": parse_key_values(profile_line(stderr, "[mt-profile] activation_delta")),
        "rejection_reasons": profile_line(stderr, "[mt-profile] rejection_reasons"),
        "batch_size_hist": profile_line(stderr, "[mt-profile] batch_size_hist="),
        "effective_worker_count_hist": profile_line(stderr, "[mt-profile] effective_worker_count_hist="),
        "partition_facts": profile_line(stderr, "[mt-profile] partition_facts"),
        "worker_task_count": profile_line(stderr, "[mt-profile] worker_task_count="),
        "task_cpp_ids": profile_line(stderr, "[mt-profile] task_cpp_ids="),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gsim-bin", required=True)
    parser.add_argument("--fir", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    gsim_bin = Path(args.gsim_bin)
    fir = Path(args.fir)
    work_dir = Path(args.work_dir)
    seq_dir = work_dir / "seq"
    mt_dir = work_dir / "mt_active_frequency"
    driver_dir = work_dir / "drivers"

    expect(gsim_bin.is_file(), f"{gsim_bin} does not exist")
    expect(fir.is_file(), f"{fir} does not exist")
    if work_dir.exists():
        shutil.rmtree(work_dir)
    seq_dir.mkdir(parents=True)
    mt_dir.mkdir(parents=True)

    base_flags = ["--supernode-max-size=1"]
    active_frequency_flags = [
        "--mt-helper-mode=mt",
        "--mt-batch-formation=active-frequency",
        "--mt-active-frequency-cost-threshold=2",
        "--dump-mt-schedule-json",
    ]
    run([str(gsim_bin), "--dir", str(seq_dir), *base_flags, "--mt-helper-mode=seq", str(fir)], timeout=args.timeout)
    run([str(gsim_bin), "--dir", str(mt_dir), *base_flags, *active_frequency_flags, str(fir)], timeout=args.timeout)
    check_structure(mt_dir)

    orword = Path(__file__).with_name("check-mt-activation-delta-orword.py")
    run([sys.executable, str(orword), str(mt_dir)], timeout=args.timeout)

    seq_bin = build_driver(seq_dir, driver_dir, "seq")
    mt_bin = build_driver(mt_dir, driver_dir, "mt_active_frequency")
    seq_result = run_driver(seq_bin, {})

    cases = [
        ("off_t1", {"GSIM_MT_WORKER_POOL": "0", "GSIM_THREADS": "1"}, 0, False),
        ("off_t4", {"GSIM_MT_WORKER_POOL": "0", "GSIM_THREADS": "4"}, 0, True),
        ("on_t2", {"GSIM_MT_WORKER_POOL": "1", "GSIM_THREADS": "2"}, 1, True),
        ("on_t4", {"GSIM_MT_WORKER_POOL": "1", "GSIM_THREADS": "4"}, 1, True),
    ]

    summaries = []
    first_pool_stdout = None
    first_pool_stderr = None
    for label, env_values, expected_pool, expect_parallel in cases:
        run_env = {
            "GSIM_MT_PROFILE": "1",
            "GSIM_MT_PROFILE_TASKS": "1",
            **env_values,
        }
        result = run_driver(mt_bin, run_env)
        main_values, delta_values, worker_line = verify_run(
            label,
            result,
            seq_result.stdout,
            expected_pool,
            expect_parallel,
        )
        if label == "on_t4":
            first_pool_stdout = result.stdout
            first_pool_stderr = result.stderr
            repeat = run_driver(mt_bin, run_env)
            verify_run("on_t4_repeat", repeat, seq_result.stdout, expected_pool, expect_parallel)
            expect(first_pool_stdout == repeat.stdout, "pool-on repeated run stdout is not deterministic")
            expect(stable_profile(first_pool_stderr) == stable_profile(repeat.stderr),
                   "pool-on repeated run stable profile counters are not deterministic")
        summaries.append(
            (
                label,
                main_values.get("worker_pool", ""),
                main_values.get("true_parallel_batch_count", ""),
                main_values.get("max_worker_count", ""),
                delta_values.get("entries", ""),
                worker_line.split("=", 1)[1],
            )
        )

    summary = ["mt-17x-worker-pool-gate ok: seq_mt_trace=matched pool_repeat=matched"]
    for label, worker_pool, true_parallel_batch_count, max_worker_count, delta_entries, worker_task_count in summaries:
        summary.append(
            f"{label}(worker_pool={worker_pool} "
            f"true_parallel_batch_count={true_parallel_batch_count} "
            f"max_worker_count={max_worker_count} "
            f"activation_delta_entries={delta_entries} "
            f"worker_task_count={worker_task_count})"
        )
    print(" ".join(summary))


if __name__ == "__main__":
    main()
