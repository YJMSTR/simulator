#!/usr/bin/env python3
import argparse
import json
import re
import sys
from pathlib import Path


def fail(message):
    print(f"mt-repcut-lite-runtime failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def load_report(model_dir):
    reports = sorted(model_dir.glob("*_mt_repcut_lite.json"))
    expect(len(reports) == 1, f"expected one repcut-lite report under {model_dir}, found {len(reports)}")
    with reports[0].open() as fp:
        return reports[0], json.load(fp)


def generated_text(model_dir):
    paths = sorted(model_dir.glob("*.h")) + sorted(model_dir.glob("*.cpp"))
    expect(paths, f"expected generated C++ files under {model_dir}")
    return "\n".join(path.read_text() for path in paths)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model_dir")
    args = parser.parse_args()

    model_dir = Path(args.model_dir)
    expect(model_dir.is_dir(), f"{model_dir} is not a directory")

    report_path, report = load_report(model_dir)
    text = generated_text(model_dir)

    expect(report.get("format") == "gsim.mt-repcut-lite.v1", f"unexpected report format in {report_path}")
    expect(report.get("mode") == "on", "runtime-applied check requires --mt-repcut-lite=on")
    expect(report.get("candidate_count", 0) > 0, "expected at least one RepCut-lite candidate")
    expect(report.get("selected_count", 0) > 0, "expected at least one selected RepCut-lite candidate")
    expect(report.get("applied_to_runtime") is True, "selected RepCut-lite candidates were not applied to runtime")

    selected = [task for task in report.get("tasks", []) if task.get("selected")]
    expect(len(selected) == report.get("selected_count"), "selected_count does not match selected task list")
    expect("mtRunPureBatch" in text, "generated runtime has no mtRunPureBatch")

    for task in selected:
        cpp_id = task.get("cpp_id")
        expect(isinstance(cpp_id, int), f"selected task has invalid cpp_id: {task}")
        expect(task.get("task_kind") == "pure_compute", f"selected task {cpp_id} is not pure_compute")
        expect(not task.get("serial_reasons"), f"selected task {cpp_id} has serial reasons")
        expect(task.get("block_reason") in (None, ""), f"selected task {cpp_id} has block reason")

        decl_pattern = rf"\bvoid\s+mtRepCutLiteTask{cpp_id}\s*\("
        body_pattern = rf"\bvoid\s+\w+::mtRepCutLiteTask{cpp_id}\s*\([^)]*ActiveBuffer[^)]*\)\s*\{{"
        call_token = f"mtRepCutLiteTask{cpp_id}(workerFlags[worker], workerBuffers[worker]);"
        expect(re.search(decl_pattern, text), f"missing copied helper declaration for task {cpp_id}")
        expect(re.search(body_pattern, text), f"missing copied helper body for task {cpp_id}")
        expect(call_token in text, f"mtRunPureBatch does not call copied helper for task {cpp_id}")

    print(f"mt-repcut-lite-runtime ok: {len(selected)} selected tasks applied from {report_path}")


if __name__ == "__main__":
    main()
