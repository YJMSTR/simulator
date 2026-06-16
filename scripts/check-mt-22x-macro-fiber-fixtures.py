#!/usr/bin/env python3
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


CASES = [
    "mt-22x-shared-pure-repcut",
    "mt-22x-high-copy-reject",
    "mt-22x-blockers",
    "mt-22x-activity-skew",
]


def fail(message):
    print(f"mt-22x-macro-fiber-fixtures failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def run(command, *, env=None, timeout=None, log_prefix=None):
    try:
        result = subprocess.run(command, text=True, capture_output=True, env=env, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        fail(f"command timed out: {' '.join(command)}\nstdout:\n{exc.stdout or ''}\nstderr:\n{exc.stderr or ''}")
    if log_prefix is not None:
        Path(str(log_prefix) + ".command.txt").write_text(" ".join(command) + "\n")
        Path(str(log_prefix) + ".stdout.txt").write_text(result.stdout)
        Path(str(log_prefix) + ".stderr.txt").write_text(result.stderr)
        Path(str(log_prefix) + ".exit.txt").write_text(str(result.returncode) + "\n")
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
    sources = sorted(model_dir.glob("*.cpp"))
    expect(sources, f"no generated C++ sources found under {model_dir}")
    return top, setters, getters, header, sources


def driver_source(top, setters, getters, header_name, case_name):
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
        "  if (lower.find(\"addr\") != std::string::npos) return cycle & 3;",
    ]
    if case_name == "mt-22x-activity-skew":
        lines.extend([
            "  if (lower.find(\"cold\") != std::string::npos) return cycle == 0 ? (uint64_t)(index + 1) : 0;",
            "  if (lower.find(\"g1\") != std::string::npos) return cycle == 0;",
            "  if (lower.find(\"hot\") != std::string::npos) return (uint64_t)((cycle + 5) * (index + 3));",
        ])
    lines.extend([
        "  if (lower.find(\"we\") != std::string::npos) return (cycle & 1) != 0;",
        "  if (lower.find(\"g\") != std::string::npos) return ((cycle + index) & 1) != 0;",
        "  return (uint64_t)((cycle + 7) * (index + 11) + ((cycle << (index % 3)) & 0xff));",
        "}",
        "",
        "int main() {",
        f"  S{top} dut;",
        "  for (int cycle = 0; cycle < 32; cycle ++) {",
    ])
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


def build_driver(model_dir, work_dir, case_name):
    top, setters, getters, header, sources = model_info(model_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    driver = work_dir / f"{case_name}_driver.cpp"
    binary = work_dir / f"{case_name}_driver"
    driver.write_text(driver_source(top, setters, getters, header.name, case_name))
    stub = work_dir / f"{case_name}_ext_stubs.cpp"
    stub_sources = []
    stub_text = extmodule_stub_source(header.read_text())
    if stub_text:
        stub.write_text(stub_text)
        stub_sources.append(str(stub))
    compiler = os.environ.get("CXX", "clang++")
    run([
        compiler,
        "-std=c++17",
        "-O0",
        "-pthread",
        "-I",
        str(model_dir),
        str(driver),
        *stub_sources,
        *[str(source) for source in sources],
        "-o",
        str(binary),
    ])
    return binary


def extmodule_stub_source(header_text):
    lines = [
        "#include <cstdint>",
        "",
    ]
    declarations = re.findall(r"^void\s+([A-Za-z_][A-Za-z0-9_]*)\(([^;]*&[^;]*)\);$", header_text, re.M)
    for name, args in declarations:
        if not name[0].isupper():
            continue
        params = [item.strip() for item in args.split(",")]
        if not params or not all(param.startswith("uint") for param in params):
            continue
        out_indices = [idx for idx, param in enumerate(params) if "&" in param]
        if not out_indices:
            continue
        named_params = []
        for idx, param in enumerate(params):
            typ = param.split()[0].replace("&", "")
            if "&" in param:
                typ += " &"
            named_params.append(f"{typ} arg{idx}")
        lines.append(f"void {name}({', '.join(named_params)}) {{")
        for idx in out_indices:
            lines.append(f"  arg{idx} = 0;")
        lines.append("}")
        lines.append("")
    return "\n".join(lines) if len(lines) > 2 else ""


def generate_case(gsim_bin, fir, model_dir, timeout, log_prefix):
    if model_dir.exists():
        shutil.rmtree(model_dir)
    model_dir.mkdir(parents=True)
    run([
        str(gsim_bin),
        "--dir",
        str(model_dir),
        "--supernode-max-size=1",
        "--mt-helper-mode=mt",
        "--mt-batch-formation=active-frequency",
        "--mt-active-frequency-cost-threshold=1",
        "--dump-mt-schedule-json",
        str(fir),
    ], timeout=timeout, log_prefix=log_prefix)


def run_profile(binary, profile_path):
    env = os.environ.copy()
    env.update({
        "GSIM_MT_PROFILE": "1",
        "GSIM_MT_PROFILE_TASKS": "1",
        "GSIM_THREADS": "4",
        "GSIM_MT_WORKER_POOL": "1",
        "GSIM_MT_MIN_BATCH_TASKS": "1",
    })
    result = run([str(binary)], env=env)
    profile_path.write_text(result.stdout + result.stderr)
    expect("[mt-profile] task_cpp_ids=" in profile_path.read_text(), f"profile missing task_cpp_ids: {profile_path}")


def run_planner(repo, model_dir, profile_path, out_dir, case_name):
    schedule_paths = sorted(model_dir.glob("*_mt_schedule.json"))
    expect(len(schedule_paths) == 1, f"expected one schedule under {model_dir}, found {len(schedule_paths)}")
    json_out = out_dir / f"{case_name}.json"
    md_out = out_dir / f"{case_name}.md"
    budget_ratio = "1.00" if case_name == "mt-22x-shared-pure-repcut" else "0.50"
    run([
        sys.executable,
        str(repo / "scripts" / "mt-22x-macro-fiber-repcut-planner.py"),
        "--schedule-json",
        str(schedule_paths[0]),
        "--profile-log",
        str(profile_path),
        "--json-out",
        str(json_out),
        "--md-out",
        str(md_out),
        "--target-partitions",
        "2,4,8,16",
        "--replication-budget-ratio",
        budget_ratio,
        "--max-duplicate-expected-cost",
        "1000000000",
    ])
    return json.loads(json_out.read_text())


def report_for(result, target):
    reports = {row["target_partitions"]: row for row in result["partition_reports"]}
    expect(target in reports, f"missing t{target} partition report")
    return reports[target]


def check_shared(result):
    t2 = report_for(result, 2)
    expect(result["totals"]["pure_candidate_expected_active_cost"] > 0, "shared fixture has no pure candidate cost")
    expect(t2["duplicated_cpp_id_count"] > 0, f"shared fixture should duplicate at least one unit: {t2}")
    expect(t2["estimated_barrier_reduction"] > 0, f"shared fixture should estimate barrier reduction: {t2}")


def check_high_copy(result):
    t2 = report_for(result, 2)
    reasons = {row["reason"] for row in t2["top_blockers"]}
    expect(t2["duplicated_cpp_id_count"] == 0 or "replication_budget" in reasons,
           f"high-copy fixture should expose budget pressure: {t2}")


def check_blockers(result):
    blockers = result["blockers"]["blocked_expected_active_cost"]
    for reason in ("memory", "reset", "state_update_proof", "external"):
        expect(reason in blockers, f"blocker fixture did not classify {reason}: {blockers}")


def check_activity(result):
    t2 = report_for(result, 2)
    expect("static_only_assignment" in t2, "activity fixture missing static-only comparison")
    expect(t2["active_weighted_assignment"]["cost_key"] == "expected_active_cost", "active assignment not activity weighted")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gsim-bin", required=True)
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--report-dir", required=True)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    gsim_bin = Path(args.gsim_bin)
    input_dir = Path(args.input_dir)
    work_dir = Path(args.work_dir)
    report_dir = Path(args.report_dir)
    expect(gsim_bin.is_file(), f"{gsim_bin} does not exist")
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True)
    report_dir.mkdir(parents=True, exist_ok=True)

    results = {}
    for case_name in CASES:
        fir = input_dir / f"{case_name}.fir"
        expect(fir.is_file(), f"missing FIR fixture {fir}")
        model_dir = work_dir / case_name / "model"
        profile_path = work_dir / case_name / f"{case_name}.profile.log"
        generate_case(gsim_bin, fir, model_dir, args.timeout, work_dir / case_name / "generate")
        binary = build_driver(model_dir, work_dir / case_name / "driver", case_name)
        run_profile(binary, profile_path)
        results[case_name] = run_planner(repo, model_dir, profile_path, report_dir, case_name)

    check_shared(results["mt-22x-shared-pure-repcut"])
    check_high_copy(results["mt-22x-high-copy-reject"])
    check_blockers(results["mt-22x-blockers"])
    check_activity(results["mt-22x-activity-skew"])
    print("mt-22x-macro-fiber-fixtures ok")


if __name__ == "__main__":
    main()
