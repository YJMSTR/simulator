#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def fail(message):
    print(f"mt-19x-coarse-dispatch-accounting failed: {message}", file=sys.stderr)
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


def generated_text(model_dir):
    paths = sorted(model_dir.glob("*.h")) + sorted(model_dir.glob("*.cpp"))
    expect(paths, f"no generated C++ sources found under {model_dir}")
    return "\n".join(path.read_text() for path in paths)


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


def build_driver(model_dir, work_dir):
    top, setters, getters, header, sources = model_info(model_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    driver = work_dir / "coarse_accounting_driver.cpp"
    binary = work_dir / "coarse_accounting_driver"
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
    fail(f"missing profile line {prefix}\nstderr:\n{stderr}")


def profile_int(values, key):
    expect(key in values, f"missing profile key {key}")
    return int(values[key].replace(",", ""))


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
    model_dir = work_dir / "model"
    driver_dir = work_dir / "drivers"
    model_dir.mkdir(parents=True, exist_ok=True)

    fir = input_dir / "mt-coarse-cross-word.fir"
    expect(fir.is_file(), f"missing FIR {fir}")
    run([
        str(gsim_bin),
        "--dir",
        str(model_dir),
        "--supernode-max-size=1",
        "--mt-helper-mode=mt",
        "--mt-batch-formation=coarse",
        "--mt-active-frequency-cost-threshold=2",
        "--dump-mt-schedule-json",
        str(fir),
    ], timeout=args.timeout)

    text = generated_text(model_dir)
    for token in (
        "mtProfileCoarseStaticRuntimeEligibleRegions",
        "mtProfileCoarseRegionInvocations",
        "mtProfileCoarseLayerDispatches",
        "mtProfileCoarseWorkerJobs",
        "mtProfileCoarseFlagWordCopies",
        "mtProfileCoarseMergeWordScans",
        "[mt-profile] coarse_dispatch",
        "[mt-profile] coarse_layer_size_hist",
        "[mt-profile] coarse_region_layer_count_hist",
    ):
        expect(token in text, f"generated coarse model missing 19X accounting token {token}")

    binary = build_driver(model_dir, driver_dir)
    env = os.environ.copy()
    env.update({
        "GSIM_MT_PROFILE": "1",
        "GSIM_MT_WORKER_POOL": "1",
        "GSIM_THREADS": "4",
    })
    result = run([str(binary)], env=env, timeout=args.timeout)
    main_values = parse_key_values(profile_line(result.stderr, "[mt-profile] helper_mode="))
    coarse_values = parse_key_values(profile_line(result.stderr, "[mt-profile] coarse_dispatch"))
    expect(profile_int(main_values, "true_parallel_batch_count") > 0,
           "fixture did not exercise coarse true-parallel runtime")
    for key in (
        "static_runtime_eligible_regions",
        "region_invocations",
        "layer_dispatches",
        "worker_jobs",
        "flag_word_copies",
        "merge_word_scans",
        "activation_delta_entries",
        "estimated_barriers",
    ):
        expect(profile_int(coarse_values, key) > 0, f"coarse accounting key {key} did not increase")
    expect(profile_int(coarse_values, "layer_dispatches") >= profile_int(coarse_values, "region_invocations"),
           "layer dispatch count must distinguish dispatches inside region invocations")
    print("mt-19x-coarse-dispatch-accounting ok: " + " ".join(f"{k}={v}" for k, v in coarse_values.items()))


if __name__ == "__main__":
    main()
