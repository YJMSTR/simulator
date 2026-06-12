#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
import json


def fail(message):
    print(f"mt-repcut-lite-parallel-clone failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def load_one(model_dir, suffix):
    paths = sorted(model_dir.glob(f"*{suffix}"))
    expect(len(paths) == 1, f"expected one *{suffix} under {model_dir}, found {len(paths)}")
    with paths[0].open() as fp:
        return paths[0], json.load(fp)


def generated_files(model_dir):
    paths = sorted(model_dir.glob("*.h")) + sorted(model_dir.glob("*.cpp"))
    expect(paths, f"expected generated C++ files under {model_dir}")
    return paths


def generated_text(model_dir):
    return "\n".join(path.read_text() for path in generated_files(model_dir))


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
    sources = sorted(model_dir.glob("*.cpp"))
    expect(setters, f"no generated input setters found in {header}")
    expect(getters, f"no generated output getters found in {header}")
    expect(sources, f"no generated C++ sources found under {model_dir}")
    return top, setters, getters, header, sources


def driver_source(top, setters, getters, header_name):
    lines = [
        "#include <cstdint>",
        "#include <iostream>",
        f"#include \"{header_name}\"",
        "",
        "static uint64_t driveValue(int cycle, int index) {",
        "  return (uint64_t)((cycle + 7) * (index + 3));",
        "}",
        "",
        "int main() {",
        f"  S{top} dut;",
        "  for (int cycle = 0; cycle < 20; cycle ++) {",
    ]
    for index, (name, typ) in enumerate(setters):
        lines.append(f"    dut.set_{name}(({typ})driveValue(cycle, {index}));")
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


def run_command(command, env=None):
    return subprocess.run(command, text=True, capture_output=True, env=env)


def compile_model(model_dir):
    top, setters, getters, header, sources = model_info(model_dir)
    work_dir = model_dir / "parallel_clone_check"
    work_dir.mkdir(exist_ok=True)
    driver = work_dir / "driver.cpp"
    binary = work_dir / "driver"
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
    expect(result.returncode == 0,
           f"compile failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return binary


def run_trace(binary, threads, require_parallel):
    env = os.environ.copy()
    env["GSIM_THREADS"] = str(threads)
    env["GSIM_MT_PROFILE"] = "1"
    result = run_command([str(binary)], env=env)
    expect(result.returncode == 0,
           f"driver failed ({result.returncode}): {binary}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    match = re.search(r"\bmax_worker_count=(\d+)\b", result.stderr)
    expect(match is not None, f"profile output missing max_worker_count:\n{result.stderr}")
    max_worker_count = int(match.group(1))
    if require_parallel:
        expect(max_worker_count > 1,
               f"expected GSIM_THREADS={threads} to use multiple workers, saw max_worker_count={max_worker_count}")
    return result.stdout, result.stderr


def compile_and_compare_traces(model_dir, reference_threads, threads):
    binary = compile_model(model_dir)
    reference_stdout, _ = run_trace(binary, reference_threads, require_parallel=False)
    parallel_stdout, _ = run_trace(binary, threads, require_parallel=True)
    expect(reference_stdout == parallel_stdout,
           f"reference trace with GSIM_THREADS={reference_threads} differs from GSIM_THREADS={threads}\n"
           f"reference:\n{reference_stdout}\nparallel:\n{parallel_stdout}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model_dir")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--reference-threads", type=int, default=1)
    args = parser.parse_args()

    expect(args.threads in (2, 4), "--threads must be 2 or 4")
    expect(args.reference_threads >= 1, "--reference-threads must be >= 1")
    expect(args.reference_threads != args.threads, "--reference-threads must differ from --threads")
    model_dir = Path(args.model_dir)
    expect(model_dir.is_dir(), f"{model_dir} is not a directory")
    report_path, report = load_one(model_dir, "_mt_repcut_lite.json")
    text = generated_text(model_dir)

    expect("boundary_candidate_count" in report,
           f"report {report_path} must expose boundary_candidate_count to disambiguate candidate_count")
    expect("selected_sink_count" in report,
           f"report {report_path} must expose selected_sink_count to disambiguate selected_count")
    expect(report.get("candidate_count") == report.get("boundary_candidate_count"),
           "candidate_count must remain an alias of boundary_candidate_count")

    cut_batches = report.get("cut_batches")
    expect(isinstance(cut_batches, list), f"report {report_path} is missing cut_batches")
    parallel_batches = [
        batch for batch in cut_batches
        if batch.get("forced_serial") is False and batch.get("parallel_safe") is True
    ]
    expect(parallel_batches, "expected at least one parallel-safe cut batch")

    duplicated_nodes = report.get("duplicated_nodes")
    expect(isinstance(duplicated_nodes, list), f"report {report_path} is missing duplicated_nodes")
    expect(duplicated_nodes, "expected at least one duplicated node")
    for item in duplicated_nodes:
      for key in ("source_cpp_id", "sink_cpp_id", "source_node", "clone_name"):
          expect(key in item, f"duplicated node entry missing {key}: {item}")
      expect(str(item["clone_name"]) in text, f"clone name {item['clone_name']} is missing from generated C++")

    clone_pairs = {
        (item.get("source_cpp_id"), item.get("sink_cpp_id"))
        for item in duplicated_nodes
    }
    cut_edge_pairs = {
        (edge.get("from_cpp_id"), edge.get("to_cpp_id"))
        for edge in report.get("cut_edges", [])
    }

    for batch in parallel_batches:
        begin = batch.get("begin_cpp_id")
        end = batch.get("end_cpp_id")
        expect(isinstance(begin, int) and isinstance(end, int) and begin < end,
               f"invalid cut batch range: {batch}")
        forced_sinks = batch.get("forced_sink_cpp_ids")
        expect(isinstance(forced_sinks, list) and forced_sinks,
               f"parallel-safe cut batch must record forced_sink_cpp_ids: {batch}")
        forced_sink_mask = batch.get("forced_sink_mask")
        expect(isinstance(forced_sink_mask, str) and re.fullmatch(r"0x[0-9a-fA-F]+", forced_sink_mask),
               f"parallel-safe cut batch must record hex forced_sink_mask: {batch}")
        computed_mask = 0
        for sink in forced_sinks:
            expect(isinstance(sink, int) and begin <= sink < end,
                   f"forced sink {sink} is outside batch {begin}->{end}")
            computed_mask |= 1 << (sink % 64)
        expect(int(forced_sink_mask, 16) == computed_mask,
               f"forced_sink_mask {forced_sink_mask} does not match forced_sink_cpp_ids {forced_sinks}")
        expect(batch.get("forced_sink_activation") is True,
               f"parallel-safe cut batch must record forced_sink_activation=true: {batch}")
        expect(batch.get("parallel_safe_reason") == "all_forced_sink_cut_inputs_cloned",
               f"parallel-safe cut batch has unexpected proof reason: {batch}")
        expect(batch.get("clone_count") == batch.get("cut_edge_count"),
               f"parallel-safe cut batch clone_count must match cut_edge_count: {batch}")
        batch_edges = [
            (from_id, to_id) for from_id, to_id in cut_edge_pairs
            if begin <= from_id < end and begin <= to_id < end
        ]
        for sink in forced_sinks:
            incoming = [(from_id, to_id) for from_id, to_id in batch_edges if to_id == sink]
            expect(incoming, f"forced sink {sink} has no incoming cut edges in batch {begin}->{end}")
            for pair in incoming:
                expect(pair in clone_pairs, f"forced sink cut edge {pair[0]}->{pair[1]} has no clone evidence")
        expect(re.search(rf"\bmtRunPureBatch\({begin},\s*{end},\s*oldFlag\);", text),
               f"generated runtime has no mtRunPureBatch({begin}, {end}, oldFlag) call")
        forced_case = re.compile(rf"\bcase\s+{begin}\s*:\s*\n\s*workerCount\s*=\s*1\s*;", re.M)
        expect(forced_case.search(text) is None,
               f"parallel-safe cut batch {begin}->{end} still forces workerCount = 1")

    compile_and_compare_traces(model_dir, args.reference_threads, args.threads)
    print(f"mt-repcut-lite-parallel-clone ok: {len(parallel_batches)} parallel-safe cut batches, "
          f"{len(duplicated_nodes)} duplicated nodes, trace GSIM_THREADS={args.reference_threads}/{args.threads} matched from {report_path}")


if __name__ == "__main__":
    main()
