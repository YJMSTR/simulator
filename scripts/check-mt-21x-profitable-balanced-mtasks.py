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
    print(f"mt-21x-profitable-balanced-mtasks failed: {message}", file=sys.stderr)
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


def generate_model(gsim_bin, fir, model_dir, mode, timeout):
    if model_dir.exists():
        shutil.rmtree(model_dir)
    model_dir.mkdir(parents=True, exist_ok=True)
    flags = ["--supernode-max-size=1"]
    if mode == "seq":
        flags.append("--mt-helper-mode=seq")
    elif mode == "profitable":
        flags.extend([
            "--mt-helper-mode=mt",
            "--mt-batch-formation=coarse",
            "--mt-active-frequency-cost-threshold=2",
            "--mt-coarse-runtime=mtask",
            "--mt-coarse-profitability=static",
            "--mt-coarse-worker-policy=profitable",
            "--dump-mt-coarse-region-report",
        ])
    else:
        fail(f"unknown model mode {mode}")
    run([str(gsim_bin), "--dir", str(model_dir), *flags, str(fir)], timeout=timeout)


def load_report(model_dir):
    reports = sorted(model_dir.glob("*_mt_coarse_regions.json"))
    expect(len(reports) == 1, f"expected one coarse-region report under {model_dir}, found {len(reports)}")
    return json.loads(reports[0].read_text())


def generated_text(model_dir):
    return "\n".join(path.read_text() for path in sorted(model_dir.glob("*.h")) + sorted(model_dir.glob("*.cpp")))


def parse_key_values(line):
    return {key: value.rstrip(",") for key, value in re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", line)}


def parse_hist(line):
    text = line.split("=", 1)[1]
    hist = {}
    for item in text.split(","):
        if not item:
            continue
        key, value = item.split(":", 1)
        hist[int(key)] = int(value)
    return hist


def profile_line(stderr, prefix):
    for line in stderr.splitlines():
        if line.startswith(prefix):
            return line
    fail(f"missing profile line {prefix}\nstderr:\n{stderr}")


def profile_int(values, key):
    expect(key in values, f"missing profile key {key}")
    return int(values[key].replace(",", ""))


def run_driver(binary, threads, profile):
    env = os.environ.copy()
    env["GSIM_THREADS"] = str(threads)
    env["GSIM_MT_WORKER_POOL"] = "1"
    if profile:
        env["GSIM_MT_PROFILE"] = "1"
    else:
        env.pop("GSIM_MT_PROFILE", None)
    return run([str(binary)], env=env)


def largest_eligible_region(report):
    regions = [region for region in report.get("regions", []) if region.get("runtime_eligible")]
    expect(regions, "report has no runtime-eligible region")
    return max(regions, key=lambda item: item.get("task_count", 0))


def check_balanced_report(report):
    expect(report.get("format") == "gsim.mt-coarse-region-report.v2", "21X report format was not v2")
    expect(report.get("coarse_worker_policy") == "profitable", "report did not record profitable worker policy")
    region = largest_eligible_region(report)
    expect(region.get("mtask_count", 0) >= 4, "balanced fixture does not expose at least four MTasks")
    rec_t4 = region["recommended_workers"]["t4"]
    expect(rec_t4["workers"] == 4 and rec_t4["admitted"], f"profitable t4 should use four workers: {rec_t4}")
    rec_t16 = region["recommended_workers"]["t16"]
    expect(rec_t16["workers"] < 16 and rec_t16["admitted"],
           f"profitable t16 should cap below GSIM_THREADS when MTasks/work are limited: {rec_t16}")
    assignment = region["mtask_assignments"]["t4"]
    expect(assignment["effective_workers"] == 4, f"t4 assignment did not use four workers: {assignment}")
    expect(assignment["balanced_worst_static_cost"] < assignment["contiguous_worst_static_cost"],
           f"balanced assignment did not reduce worst static cost: {assignment}")
    expect(len(assignment["worker_mtask_indices"]) == 4, f"missing four worker assignment lists: {assignment}")
    expect(all(assignment["worker_mtask_indices"]), f"each t4 worker should receive at least one MTask: {assignment}")


def check_cap_report(report):
    region = largest_eligible_region(report)
    rec_t16 = region["recommended_workers"]["t16"]
    expect(rec_t16["workers"] < 16, f"cap fixture should not select all t16 workers: {rec_t16}")
    assignment = region["mtask_assignments"]["t16"]
    expect(assignment["effective_workers"] == rec_t16["workers"],
           f"assignment and recommendation disagree: {assignment} vs {rec_t16}")


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
    balanced_fir = input_dir / "mt-coarse-balanced-mtasks.fir"
    cap_fir = input_dir / "mt-coarse-cross-word.fir"
    reject_fir = input_dir / "mt-coarse-serial-split.fir"
    expect(gsim_bin.is_file(), f"{gsim_bin} does not exist")
    for fir in (balanced_fir, cap_fir, reject_fir):
        expect(fir.is_file(), f"missing FIR {fir}")
    if work_dir.exists():
        shutil.rmtree(work_dir)

    models = {
        "seq_balanced": work_dir / "models" / "seq_balanced",
        "balanced": work_dir / "models" / "balanced",
        "seq_cap": work_dir / "models" / "seq_cap",
        "cap": work_dir / "models" / "cap",
        "seq_reject": work_dir / "models" / "seq_reject",
        "reject": work_dir / "models" / "reject",
    }
    generate_model(gsim_bin, balanced_fir, models["seq_balanced"], "seq", args.timeout)
    generate_model(gsim_bin, balanced_fir, models["balanced"], "profitable", args.timeout)
    generate_model(gsim_bin, cap_fir, models["seq_cap"], "seq", args.timeout)
    generate_model(gsim_bin, cap_fir, models["cap"], "profitable", args.timeout)
    generate_model(gsim_bin, reject_fir, models["seq_reject"], "seq", args.timeout)
    generate_model(gsim_bin, reject_fir, models["reject"], "profitable", args.timeout)

    check_balanced_report(load_report(models["balanced"]))
    check_cap_report(load_report(models["cap"]))
    reject_report = load_report(models["reject"])
    expect(reject_report.get("runtime_eligible_region_count", 0) == 0,
           "rejected fixture should have no coarse runtime-eligible region")

    text = generated_text(models["balanced"])
    for token in (
        "mtRunCoarseMTaskWorkerList",
        "mtBuildCoarseMTaskWorkerAssignment",
        "coarse_worker_policy=%s",
        "coarse_assignment",
    ):
        expect(token in text, f"generated profitable model missing token {token}")

    binaries = {
        name: build_driver(model_dir, work_dir / "drivers", name)
        for name, model_dir in models.items()
    }

    for seq_name, mt_name in (("seq_balanced", "balanced"), ("seq_cap", "cap"), ("seq_reject", "reject")):
        reference = run_driver(binaries[seq_name], 1, False).stdout
        for threads in (1, 2, 4, 8, 16):
            result = run_driver(binaries[mt_name], threads, False)
            expect(result.stdout == reference, f"{mt_name} t{threads} trace differed from seq reference")

    balanced_profile = run_driver(binaries["balanced"], 4, True).stderr
    dispatch = parse_key_values(profile_line(balanced_profile, "[mt-profile] coarse_dispatch"))
    assignment = parse_key_values(profile_line(balanced_profile, "[mt-profile] coarse_assignment"))
    selected_hist = parse_hist(profile_line(balanced_profile, "[mt-profile] coarse_selected_worker_count_hist="))
    expect(dispatch.get("coarse_worker_policy") == "profitable", "profile did not record profitable policy")
    expect(profile_int(dispatch, "accepted_regions") > 0, "balanced fixture did not accept coarse MTask regions")
    expect(profile_int(dispatch, "mtask_dispatches") > 0, "balanced fixture did not dispatch MTasks")
    expect(profile_int(dispatch, "active_mtasks") > 0, "profile did not record active MTasks")
    expect(selected_hist.get(4, 0) > 0, f"t4 balanced profile did not use four workers: {selected_hist}")
    expect(profile_int(assignment, "balanced_worst_static_cost") < profile_int(assignment, "contiguous_worst_static_cost"),
           f"profile did not record balanced improvement: {assignment}")

    cap_profile = run_driver(binaries["cap"], 16, True).stderr
    cap_dispatch = parse_key_values(profile_line(cap_profile, "[mt-profile] coarse_dispatch"))
    cap_hist = parse_hist(profile_line(cap_profile, "[mt-profile] coarse_selected_worker_count_hist="))
    expect(cap_dispatch.get("coarse_worker_policy") == "profitable", "cap profile did not record profitable policy")
    expect(not any(worker >= 16 and count > 0 for worker, count in cap_hist.items()),
           f"cap fixture should select fewer than GSIM_THREADS=16 workers: {cap_hist}")

    print("mt-21x-profitable-balanced-mtasks ok: "
          f"balanced_selected={selected_hist} cap_selected={cap_hist} "
          f"balanced_worst={assignment['balanced_worst_static_cost']} "
          f"contiguous_worst={assignment['contiguous_worst_static_cost']}")


if __name__ == "__main__":
    main()
