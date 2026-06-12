#!/usr/bin/env python3
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


REFERENCE_MODES = ["off", "seq", "buffered-seq"]


def fail(message):
    print(f"mt-fir-smoke failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def split_flags(flags):
    return shlex.split(flags) if flags.strip() else []


def strip_mode_flags(flags):
    stripped = []
    skip_next = False
    for flag in flags:
        if skip_next:
            skip_next = False
            continue
        if flag == "--mt-helper-mode":
            skip_next = True
            continue
        if flag.startswith("--mt-helper-mode="):
            continue
        stripped.append(flag)
    return stripped


def requested_mode(flags):
    mode = "off"
    for index, flag in enumerate(flags):
        if flag == "--mt-helper-mode" and index + 1 < len(flags):
            mode = flags[index + 1]
        elif flag.startswith("--mt-helper-mode="):
            mode = flag.split("=", 1)[1]
    return mode


def flags_for_mode(base_flags, mode):
    flags = strip_mode_flags(base_flags)
    if mode != "off":
        flags.append(f"--mt-helper-mode={mode}")
    return flags


def mode_label(mode, extra=""):
    safe = mode.replace("-", "_")
    if extra:
        safe += "_" + re.sub(r"[^A-Za-z0-9_]+", "_", extra).strip("_")
    return safe


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
        "  if (lower == \"reset\" || lower.find(\"reset\") != std::string::npos) return cycle < 2 || cycle == 5 || cycle == 9;",
        "  if (lower.find(\"we\") != std::string::npos) return (cycle % 3) == 1;",
        "  if (lower.find(\"en\") != std::string::npos) return (cycle % 4) != 0;",
        "  if (lower.find(\"gate\") != std::string::npos) return (cycle % 3) != 0;",
        "  if (lower.find(\"addr\") != std::string::npos) return cycle & 3;",
        "  if (lower.find(\"sel\") != std::string::npos) return cycle & 1;",
        "  return (uint64_t)((cycle + 3) * (index + 5));",
        "}",
        "",
        "int main() {",
        f"  S{top} dut;",
        "  for (int cycle = 0; cycle < 20; cycle ++) {",
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


def run_command(command, timeout=None):
    try:
        return subprocess.run(command, text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        fail(f"command timed out: {' '.join(command)}\nstdout:\n{exc.stdout or ''}\nstderr:\n{exc.stderr or ''}")


def generate_model(gsim_bin, input_path, model_dir, flags, timeout):
    if model_dir.exists():
        shutil.rmtree(model_dir)
    model_dir.mkdir(parents=True)
    command = [str(gsim_bin), "--dir", str(model_dir), *flags, str(input_path)]
    result = run_command(command, timeout)
    (model_dir / "gsim.stdout.log").write_text(result.stdout)
    (model_dir / "gsim.stderr.log").write_text(result.stderr)
    if result.returncode != 0:
        fail(f"command failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")


def build_and_run_trace(model_dir, work_dir, label):
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
    result = run_command(command)
    if result.returncode != 0:
        fail(f"compile failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    result = run_command([str(binary)])
    if result.returncode != 0:
        fail(f"driver failed ({result.returncode}): {binary}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    if result.stderr:
        (work_dir / f"{label}_driver.stderr.log").write_text(result.stderr)
        sys.stderr.write(result.stderr)
    return result.stdout


def check_mt_structure(model_dir, case):
    text = "\n".join(path.read_text() for path in sorted(model_dir.glob("*.h")) + sorted(model_dir.glob("*.cpp")))
    for token in ("#include <thread>", "ActiveBuffer", "mtRunPureBatch", "std::thread"):
        expect(token in text, f"mt generated sources missing {token}")
    batch_call_count = text.count("mtRunPureBatch(") - 1
    if case == "mt-comb":
        expect(batch_call_count >= 1, "mt-comb must dispatch at least one pure batch")
    task_bodies = re.findall(r"\bvoid\s+\w+::(mtTask\d+)\s*\([^)]*ActiveBuffer[^)]*\)\s*\{(.*?)\n\}", text, re.S)
    expect(task_bodies, "mt generated sources have no buffered mtTask helpers")
    for task, body in task_bodies:
        expect("activeFlags[" not in body, f"{task} touches global activeFlags directly")
        expect("activateAll()" not in body, f"{task} calls activateAll directly")
        expect(not re.search(r"\bsubReset\d+\(\);", body), f"{task} calls serial reset helper directly")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gsim-bin", required=True)
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--cases", required=True)
    parser.add_argument("--flags", default="")
    parser.add_argument("--timeout", default="")
    args = parser.parse_args()

    gsim_bin = Path(args.gsim_bin)
    input_dir = Path(args.input_dir)
    output_dir = Path(args.output_dir)
    cases = args.cases.split()
    base_flags = split_flags(args.flags)
    req_mode = requested_mode(base_flags)
    timeout = int(args.timeout[:-1]) if args.timeout.endswith("s") and args.timeout[:-1].isdigit() else None
    output_label = mode_label(req_mode, os.environ.get("GSIM_THREADS", "") if req_mode == "mt" else "")

    expect(gsim_bin.is_file(), f"{gsim_bin} does not exist")
    expect(cases, "no smoke cases were provided")
    expect(req_mode in {"off", "seq", "buffered-seq", "mt"}, f"unsupported requested mode {req_mode}")

    modes = list(REFERENCE_MODES)
    if req_mode not in modes:
        modes.append(req_mode)

    for case in cases:
        input_path = input_dir / f"{case}.fir"
        expect(input_path.is_file(), f"missing smoke FIR {input_path}")

        traces = {}
        case_root = output_dir / output_label / case
        for mode in modes:
            label = mode_label(mode, os.environ.get("GSIM_THREADS", "") if mode == "mt" else "")
            model_dir = case_root / label
            generate_model(gsim_bin, input_path, model_dir, flags_for_mode(base_flags, mode), timeout)
            if mode == "mt":
                check_mt_structure(model_dir, case)
            traces[mode] = build_and_run_trace(model_dir, case_root / "drivers", label)
            (model_dir / "trace.txt").write_text(traces[mode])

        expect(traces["off"] == traces["seq"], f"{case}: off and seq traces differ")
        expect(traces["seq"] == traces["buffered-seq"], f"{case}: seq and buffered-seq traces differ")
        if req_mode == "mt":
            expect(traces["seq"] == traces["mt"], f"{case}: seq and mt traces differ")

        print(f"{case}: {req_mode} smoke trace matched references")

    print(f"mt FIR smoke passed for mode {req_mode} on {len(cases)} cases")


if __name__ == "__main__":
    main()
