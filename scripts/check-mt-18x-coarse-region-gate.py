#!/usr/bin/env python3
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def fail(message):
    print(f"mt-18x-coarse-region-gate failed: {message}", file=sys.stderr)
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
        "  if (lower.find(\"reset\") != std::string::npos) return cycle < 2;",
        "  if (lower.find(\"io_g\") != std::string::npos) return ((cycle + index) % 3) != 0;",
        "  return (uint64_t)((cycle + 7) * (index + 11) + ((cycle << (index % 3)) & 0xff));",
        "}",
        "",
        "int main() {",
        f"  S{top} dut;",
        "  for (int cycle = 0; cycle < 32; cycle ++) {",
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


def generated_text(model_dir):
    return "\n".join(path.read_text() for path in sorted(model_dir.glob("*.h")) + sorted(model_dir.glob("*.cpp")))


def load_report(model_dir):
    reports = sorted(model_dir.glob("*_mt_coarse_regions.json"))
    expect(len(reports) == 1, f"expected one coarse-region report under {model_dir}, found {len(reports)}")
    return reports[0], json.loads(reports[0].read_text())


def parse_key_values(line):
    return {key: value.rstrip(",") for key, value in re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", line)}


def profile_line(stderr, prefix):
    for line in stderr.splitlines():
        if line.startswith(prefix):
            return line
    fail(f"missing profile line {prefix}\nstderr:\n{stderr}")


def profile_int(values, key):
    expect(key in values, f"missing profile key {key}")
    return int(values[key].replace(",", ""))


def run_driver(binary, env_overrides):
    env = os.environ.copy()
    env.update(env_overrides)
    return run([str(binary)], env=env)


def check_coarse_structure(model_dir):
    text = generated_text(model_dir)
    for token in (
        "mtRunCoarseRegion",
        "mtRunCoarseLayerWorkerRange",
        "mtWorkerCoarseFlags",
        "ActivationDelta",
        "mtWorkerPoolLoop",
    ):
        expect(token in text, f"generated coarse model missing {token}")
    expect(re.search(r"mtRunCoarseRegion\(\d+,\s*mtCoarseWords\d+\);", text),
           "generated coarse model has no coarse-region dispatch")


def check_cross_word_report(report):
    expect(report.get("format") == "gsim.mt-coarse-region-report.v1", "unexpected coarse report format")
    expect(report.get("mode") == "coarse", "coarse report mode mismatch")
    regions = report.get("regions") or []
    eligible = [region for region in regions if region.get("runtime_eligible")]
    expect(eligible, "expected at least one runtime-eligible coarse region")
    best = max(eligible, key=lambda region: region.get("task_count", 0))
    expect(best.get("active_word_span", 0) > 1, f"coarse region did not cross active words: {best}")
    expect(best.get("task_count", 0) > 8, f"coarse region is not larger than ACTIVE_WIDTH: {best}")
    expect(best.get("estimated_max_parallel_width", 0) >= 4, f"coarse width too small: {best}")
    expect(best.get("estimated_layer_count", 0) >= 1, f"missing layer estimate: {best}")
    expect(isinstance(best.get("layers"), list) and best["layers"], "missing coarse layers")


def check_serial_split_report(report):
    regions = report.get("regions") or []
    blocker_counts = report.get("blocker_counts") or {}
    expect(report.get("runtime_eligible_region_count", 0) == 0,
           "serial-split fixture should not form a runtime-eligible coarse region")
    expect(blocker_counts.get("serial_task", 0) > 0 or not regions,
           "serial-split fixture did not classify serial task blocker")


def verify_cross_word_runtime(seq_bin, coarse_bin):
    seq_result = run_driver(seq_bin, {})
    summaries = []
    for threads in (1, 2, 4, 8, 16):
        result = run_driver(
            coarse_bin,
            {
                "GSIM_MT_PROFILE": "1",
                "GSIM_MT_PROFILE_TASKS": "1",
                "GSIM_MT_WORKER_POOL": "1",
                "GSIM_THREADS": str(threads),
            },
        )
        expect(result.stdout == seq_result.stdout,
               f"coarse trace differs from seq trace at GSIM_THREADS={threads}")
        main_values = parse_key_values(profile_line(result.stderr, "[mt-profile] helper_mode="))
        delta_values = parse_key_values(profile_line(result.stderr, "[mt-profile] activation_delta"))
        if threads > 1:
            expect(profile_int(main_values, "true_parallel_batch_count") > 0,
                   f"coarse profile did not enter true-parallel batches at GSIM_THREADS={threads}")
            expect(profile_int(main_values, "max_worker_count") > 1,
                   f"coarse profile did not use more than one worker at GSIM_THREADS={threads}")
        summaries.append(
            f"t{threads}(true_parallel_batch_count={main_values.get('true_parallel_batch_count')} "
            f"max_worker_count={main_values.get('max_worker_count')} "
            f"activation_delta_entries={delta_values.get('entries')})"
        )
    return summaries


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gsim-bin", required=True)
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    gsim_bin = Path(args.gsim_bin)
    input_dir = Path(args.input_dir)
    work_dir = Path(args.work_dir)
    expect(gsim_bin.is_file(), f"{gsim_bin} does not exist")
    if work_dir.exists():
        shutil.rmtree(work_dir)
    seq_dir = work_dir / "seq_cross_word"
    coarse_dir = work_dir / "coarse_cross_word"
    split_dir = work_dir / "coarse_serial_split"
    driver_dir = work_dir / "drivers"
    for path in (seq_dir, coarse_dir, split_dir):
        path.mkdir(parents=True, exist_ok=True)

    cross_fir = input_dir / "mt-coarse-cross-word.fir"
    split_fir = input_dir / "mt-coarse-serial-split.fir"
    expect(cross_fir.is_file(), f"missing FIR {cross_fir}")
    expect(split_fir.is_file(), f"missing FIR {split_fir}")

    base_flags = ["--supernode-max-size=1"]
    coarse_flags = [
        "--mt-helper-mode=mt",
        "--mt-batch-formation=coarse",
        "--mt-active-frequency-cost-threshold=2",
        "--dump-mt-schedule-json",
    ]
    run([str(gsim_bin), "--dir", str(seq_dir), *base_flags, "--mt-helper-mode=seq", str(cross_fir)], timeout=args.timeout)
    run([str(gsim_bin), "--dir", str(coarse_dir), *base_flags, *coarse_flags, str(cross_fir)], timeout=args.timeout)
    run([str(gsim_bin), "--dir", str(split_dir), *base_flags, *coarse_flags, str(split_fir)], timeout=args.timeout)

    check_coarse_structure(coarse_dir)
    _, cross_report = load_report(coarse_dir)
    _, split_report = load_report(split_dir)
    check_cross_word_report(cross_report)
    check_serial_split_report(split_report)

    seq_bin = build_driver(seq_dir, driver_dir, "seq_cross_word")
    coarse_bin = build_driver(coarse_dir, driver_dir, "coarse_cross_word")
    summaries = verify_cross_word_runtime(seq_bin, coarse_bin)
    print("mt-18x-coarse-region-gate ok: seq_coarse_trace=matched " + " ".join(summaries))


if __name__ == "__main__":
    main()
