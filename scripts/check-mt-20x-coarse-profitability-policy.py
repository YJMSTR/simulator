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
    print(f"mt-20x-coarse-profitability-policy failed: {message}", file=sys.stderr)
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
    elif mode == "accepted":
        flags.extend([
            "--mt-helper-mode=mt",
            "--mt-batch-formation=coarse",
            "--mt-active-frequency-cost-threshold=2",
            "--mt-coarse-runtime=mtask",
            "--mt-coarse-profitability=static",
            "--dump-mt-coarse-region-report",
        ])
    elif mode == "rejected":
        flags.extend([
            "--mt-helper-mode=mt",
            "--mt-batch-formation=coarse",
            "--mt-active-frequency-cost-threshold=2",
            "--mt-coarse-runtime=mtask",
            "--mt-coarse-profitability=static",
            "--dump-mt-coarse-region-report",
        ])
    else:
        fail(f"unknown model mode {mode}")
    run([str(gsim_bin), "--dir", str(model_dir), *flags, str(fir)], timeout=timeout)


def load_report(model_dir):
    reports = sorted(model_dir.glob("*_mt_coarse_regions.json"))
    expect(len(reports) == 1, f"expected one coarse-region report under {model_dir}, found {len(reports)}")
    return json.loads(reports[0].read_text())


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


def check_report(report):
    expect(report.get("coarse_profitability") == "static", "report did not record static profitability mode")
    regions = [region for region in report.get("regions", []) if region.get("runtime_eligible")]
    expect(regions, "accepted fixture produced no runtime-eligible region")
    region = max(regions, key=lambda item: item.get("task_count", 0))
    rec = region.get("static_recommended_workers") or {}
    expect(rec.get("t16", {}).get("workers", 16) < 16, f"static policy did not recommend a cap below t16: {rec}")
    expect(region.get("mtask_count", 0) > 1, "accepted fixture has no independent MTasks")
    expect(region.get("estimated_useful_work", 0) > 0, "missing estimated useful work")


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
    accept_fir = input_dir / "mt-coarse-cross-word.fir"
    reject_fir = input_dir / "mt-coarse-serial-split.fir"
    expect(gsim_bin.is_file(), f"{gsim_bin} does not exist")
    expect(accept_fir.is_file(), f"missing FIR {accept_fir}")
    expect(reject_fir.is_file(), f"missing FIR {reject_fir}")
    if work_dir.exists():
        shutil.rmtree(work_dir)

    models = {
        "seq_accept": work_dir / "models" / "seq_accept",
        "accepted": work_dir / "models" / "accepted",
        "seq_reject": work_dir / "models" / "seq_reject",
        "rejected": work_dir / "models" / "rejected",
    }
    generate_model(gsim_bin, accept_fir, models["seq_accept"], "seq", args.timeout)
    generate_model(gsim_bin, accept_fir, models["accepted"], "accepted", args.timeout)
    generate_model(gsim_bin, reject_fir, models["seq_reject"], "seq", args.timeout)
    generate_model(gsim_bin, reject_fir, models["rejected"], "rejected", args.timeout)

    check_report(load_report(models["accepted"]))
    reject_report = load_report(models["rejected"])
    expect(reject_report.get("runtime_eligible_region_count", 0) == 0,
           "rejected fixture should have no coarse runtime-eligible region")

    binaries = {
        name: build_driver(model_dir, work_dir / "drivers", name)
        for name, model_dir in models.items()
    }

    accept_ref = run_driver(binaries["seq_accept"], 1, False).stdout
    for threads in (1, 2, 4, 8, 16):
        result = run_driver(binaries["accepted"], threads, False)
        expect(result.stdout == accept_ref, f"accepted static trace differed at GSIM_THREADS={threads}")

    reject_ref = run_driver(binaries["seq_reject"], 1, False).stdout
    for threads in (1, 2, 4, 8, 16):
        result = run_driver(binaries["rejected"], threads, False)
        expect(result.stdout == reject_ref, f"rejected fallback trace differed at GSIM_THREADS={threads}")

    accepted_profile = run_driver(binaries["accepted"], 16, True).stderr
    accepted_dispatch = parse_key_values(profile_line(accepted_profile, "[mt-profile] coarse_dispatch"))
    accepted_hist = parse_hist(profile_line(accepted_profile, "[mt-profile] coarse_selected_worker_count_hist="))
    expect(accepted_dispatch.get("coarse_profitability") == "static", "profile did not record static mode")
    expect(profile_int(accepted_dispatch, "accepted_regions") > 0, "accepted region did not use MTask runtime")
    expect(profile_int(accepted_dispatch, "mtask_dispatches") > 0, "accepted region did not dispatch MTasks")
    expect(profile_int(accepted_dispatch, "rejected_regions") == 0, "accepted fixture unexpectedly rejected regions")
    expect(any(worker > 1 and count > 0 for worker, count in accepted_hist.items()),
           f"accepted fixture did not use more than one worker: {accepted_hist}")
    expect(not any(worker >= 16 and count > 0 for worker, count in accepted_hist.items()),
           f"static policy did not cap selected workers below GSIM_THREADS=16: {accepted_hist}")

    rejected_profile = run_driver(binaries["rejected"], 16, True).stderr
    rejected_dispatch = parse_key_values(profile_line(rejected_profile, "[mt-profile] helper_mode="))
    expect(profile_int(rejected_dispatch, "true_parallel_batch_count") == 0,
           "fallback fixture unexpectedly entered true-parallel runtime")
    print("mt-20x-coarse-profitability-policy ok: "
          f"accepted_workers={accepted_hist} "
          f"accepted_regions={accepted_dispatch['accepted_regions']} "
          f"mtask_dispatches={accepted_dispatch['mtask_dispatches']}")


if __name__ == "__main__":
    main()
