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
    print(f"mt-19x-coarse-mtask-runtime failed: {message}", file=sys.stderr)
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
    else:
        flags.extend([
            "--mt-helper-mode=mt",
            "--mt-batch-formation=coarse",
            "--mt-active-frequency-cost-threshold=2",
            f"--mt-coarse-runtime={mode}",
            "--dump-mt-coarse-region-report",
        ])
    run([str(gsim_bin), "--dir", str(model_dir), *flags, str(fir)], timeout=timeout)


def generated_text(model_dir):
    return "\n".join(path.read_text() for path in sorted(model_dir.glob("*.h")) + sorted(model_dir.glob("*.cpp")))


def load_coarse_report(model_dir):
    reports = sorted(model_dir.glob("*_mt_coarse_regions.json"))
    expect(len(reports) == 1, f"expected one coarse report under {model_dir}, found {len(reports)}")
    return json.loads(reports[0].read_text())


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


def run_driver(binary, threads, profile):
    env = os.environ.copy()
    env["GSIM_THREADS"] = str(threads)
    env["GSIM_MT_WORKER_POOL"] = "1"
    if profile:
        env["GSIM_MT_PROFILE"] = "1"
    else:
        env.pop("GSIM_MT_PROFILE", None)
    return run([str(binary)], env=env)


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
    fir = input_dir / "mt-coarse-cross-word.fir"
    expect(gsim_bin.is_file(), f"{gsim_bin} does not exist")
    expect(fir.is_file(), f"missing FIR {fir}")
    if work_dir.exists():
        shutil.rmtree(work_dir)

    models = {mode: work_dir / "models" / mode for mode in ("seq", "layered", "mtask")}
    for mode, model_dir in models.items():
        generate_model(gsim_bin, fir, model_dir, mode, args.timeout)

    mtask_text = generated_text(models["mtask"])
    for token in (
        "mtRunCoarseMTaskWorkerRange",
        "mtMergeLocalCoarseDelta",
        "mtProfileCoarseMTaskDispatches",
        "coarse_runtime=%s",
    ):
        expect(token in mtask_text, f"mtask generated model missing token {token}")

    report = load_coarse_report(models["mtask"])
    eligible = [region for region in report["regions"] if region["runtime_eligible"]]
    expect(eligible, "fixture produced no runtime-eligible coarse region")
    mtask_regions = [region for region in eligible if region.get("mtask_count", 0) > 0]
    expect(mtask_regions, "mtask report did not include mtask groups")
    expect(any(any(task.get("ordering_edges_inside", 0) > 0 for task in region.get("mtasks", []))
               for region in mtask_regions),
           "fixture did not preserve an active/dependency boundary inside a deterministic mtask")
    expect(any(region.get("active_visibility_edges", 0) > 0 for region in mtask_regions),
           "fixture did not exercise active-visibility boundaries")
    expect(any(region.get("mtask_count", 0) > 1 for region in mtask_regions),
           "fixture did not keep independent mtask groups separable")

    binaries = {
        mode: build_driver(model_dir, work_dir / "drivers", mode)
        for mode, model_dir in models.items()
    }
    reference = run_driver(binaries["seq"], 1, False).stdout
    for mode in ("layered", "mtask"):
        for threads in (1, 2, 4, 8, 16):
            result = run_driver(binaries[mode], threads, False)
            expect(result.stdout == reference, f"{mode} t{threads} trace differed from seq reference")

    layered_profile = parse_key_values(profile_line(run_driver(binaries["layered"], 4, True).stderr,
                                                    "[mt-profile] coarse_dispatch"))
    mtask_profile = parse_key_values(profile_line(run_driver(binaries["mtask"], 4, True).stderr,
                                                  "[mt-profile] coarse_dispatch"))
    expect(layered_profile.get("coarse_runtime") == "layered", "layered profile did not report layered runtime")
    expect(mtask_profile.get("coarse_runtime") == "mtask", "mtask profile did not report mtask runtime")
    expect(profile_int(layered_profile, "layer_dispatches") > profile_int(layered_profile, "region_invocations"),
           "layered fixture did not exercise per-layer dispatches")
    expect(profile_int(mtask_profile, "mtask_dispatches") > 0, "mtask dispatch count did not increase")
    expect(profile_int(mtask_profile, "layer_dispatches") == 0, "mtask runtime still performed layer dispatches")
    expect(profile_int(mtask_profile, "estimated_barriers") < profile_int(layered_profile, "estimated_barriers"),
           "mtask runtime did not reduce estimated barriers")
    expect(profile_int(mtask_profile, "flag_word_copies") < profile_int(layered_profile, "flag_word_copies"),
           "mtask runtime did not reduce coarse flag word copies")
    expect(profile_int(mtask_profile, "merge_word_scans") < profile_int(layered_profile, "merge_word_scans"),
           "mtask runtime did not reduce merge word scans")
    expect(profile_int(mtask_profile, "activation_delta_entries") > 0,
           "mtask runtime lost ActivationDelta entry accounting during local merges")
    expect(profile_int(mtask_profile, "activation_delta_entries") == profile_int(layered_profile, "activation_delta_entries"),
           "mtask runtime changed ActivationDelta entry accounting relative to layered runtime")
    print("mt-19x-coarse-mtask-runtime ok: "
          f"layered_barriers={layered_profile['estimated_barriers']} "
          f"mtask_barriers={mtask_profile['estimated_barriers']} "
          f"layered_copies={layered_profile['flag_word_copies']} "
          f"mtask_copies={mtask_profile['flag_word_copies']}")


if __name__ == "__main__":
    main()
