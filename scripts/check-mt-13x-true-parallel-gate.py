#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def fail(message):
    print(f"mt-13x-true-parallel-gate failed: {message}", file=sys.stderr)
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
    command = [
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
    ]
    run(command)
    return binary


def parse_key_values(line):
    values = {}
    for key, value in re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", line):
        values[key] = value.rstrip(",")
    return values


def profile_line(stderr, prefix):
    for line in stderr.splitlines():
        if line.startswith(prefix):
            return line
    fail(f"missing profile line {prefix}")


def profile_int(values, key):
    expect(key in values, f"missing profile key {key}")
    return int(values[key].replace(",", ""))


def generated_text(model_dir):
    return "\n".join(path.read_text() for path in sorted(model_dir.glob("*.h")) + sorted(model_dir.glob("*.cpp")))


def check_structure(model_dir):
    text = generated_text(model_dir)
    expect("std::thread" in text, "generated MT model has no std::thread true-parallel path")
    expect("ActivationDelta" in text, "generated MT model has no ActivationDelta")
    expect("mtRunPureBatch" in text, "generated MT model has no mtRunPureBatch")
    expect(re.search(r"mtRunPureBatch\(\d+,\s*\d+,\s*oldFlag\);", text),
           "generated MT model has no pure batch dispatch")
    expect(re.search(r"nextActive\.orWord\(2,\s*[^;]+?\);\s*//\s*(?:\d+\s+)*\d+", text),
           "fixture does not emit cross-active-word ActivationDelta::orWord calls")


def run_driver(binary, env_overrides):
    env = os.environ.copy()
    env.update(env_overrides)
    return run([str(binary)], env=env)


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
    buffered_seq_dir = work_dir / "buffered_seq"
    mt_dir = work_dir / "mt"
    driver_dir = work_dir / "drivers"

    expect(gsim_bin.is_file(), f"{gsim_bin} does not exist")
    expect(fir.is_file(), f"{fir} does not exist")
    if work_dir.exists():
        shutil.rmtree(work_dir)
    seq_dir.mkdir(parents=True)
    buffered_seq_dir.mkdir(parents=True)
    mt_dir.mkdir(parents=True)

    base_flags = ["--supernode-max-size=1"]
    run([str(gsim_bin), "--dir", str(seq_dir), *base_flags, "--mt-helper-mode=seq", str(fir)], timeout=args.timeout)
    run([str(gsim_bin), "--dir", str(buffered_seq_dir), *base_flags, "--mt-helper-mode=buffered-seq", str(fir)], timeout=args.timeout)
    run([str(gsim_bin), "--dir", str(mt_dir), *base_flags, "--mt-helper-mode=mt", "--dump-mt-schedule-json", str(fir)], timeout=args.timeout)
    check_structure(mt_dir)

    seq_bin = build_driver(seq_dir, driver_dir, "seq")
    buffered_seq_bin = build_driver(buffered_seq_dir, driver_dir, "buffered_seq")
    mt_bin = build_driver(mt_dir, driver_dir, "mt")
    seq_result = run_driver(seq_bin, {})
    buffered_seq_result = run_driver(buffered_seq_bin, {})
    expect(seq_result.stdout == buffered_seq_result.stdout, "buffered-seq trace differs from seq trace")

    profile_summaries = []
    for thread_count in (2, 4):
        mt_result = run_driver(
            mt_bin,
            {
                "GSIM_MT_PROFILE": "1",
                "GSIM_THREADS": str(thread_count),
                "GSIM_MT_MIN_BATCH_TASKS": "1",
            },
        )
        expect(seq_result.stdout == mt_result.stdout, f"MT trace differs from seq trace at GSIM_THREADS={thread_count}")

        main_line = profile_line(mt_result.stderr, "[mt-profile] helper_mode=")
        delta_line = profile_line(mt_result.stderr, "[mt-profile] activation_delta")
        worker_line = profile_line(mt_result.stderr, "[mt-profile] worker_task_count=")
        main_values = parse_key_values(main_line)
        delta_values = parse_key_values(delta_line)
        expect(profile_int(main_values, "true_parallel_batch_count") > 0,
               f"profile did not enter true-parallel batches at GSIM_THREADS={thread_count}")
        expect(profile_int(main_values, "max_worker_count") > 1,
               f"profile did not use more than one worker at GSIM_THREADS={thread_count}")
        expect(profile_int(delta_values, "entries") > 0,
               f"profile did not record ActivationDelta entries at GSIM_THREADS={thread_count}")
        counts = [int(value) for value in worker_line.split("=", 1)[1].split(",") if value.strip()]
        expect(sum(1 for value in counts if value > 0) > 1,
               f"profile worker_task_count shows only one active worker at GSIM_THREADS={thread_count}")
        profile_summaries.append(
            (
                thread_count,
                main_values["true_parallel_batch_count"],
                main_values["max_worker_count"],
                delta_values["entries"],
                worker_line.split("=", 1)[1],
            )
        )

    summary_bits = [
        "mt-13x-true-parallel-gate ok: seq_buffered_mt_trace=matched"
    ]
    for thread_count, true_parallel_batch_count, max_worker_count, delta_entries, worker_task_count in profile_summaries:
        summary_bits.append(
            f"t{thread_count}(true_parallel_batch_count={true_parallel_batch_count} "
            f"max_worker_count={max_worker_count} "
            f"activation_delta_entries={delta_entries} "
            f"worker_task_count={worker_task_count})"
        )

    print(" ".join(summary_bits))


if __name__ == "__main__":
    main()
