#!/usr/bin/env python3
import argparse
import json
import re
import sys
from pathlib import Path


def fail(message):
    print(f"mt-repcut-lite-cut failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def load_one(model_dir, suffix):
    paths = sorted(model_dir.glob(f"*{suffix}"))
    expect(len(paths) == 1, f"expected one *{suffix} under {model_dir}, found {len(paths)}")
    with paths[0].open() as fp:
        return paths[0], json.load(fp)


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
    schedule_path, schedule = load_one(model_dir, "_mt_schedule.json")
    report_path, report = load_one(model_dir, "_mt_repcut_lite.json")
    text = generated_text(model_dir)

    tasks = {task["cpp_id"]: task for task in schedule.get("tasks", [])}
    report_tasks = {task["cpp_id"]: task for task in report.get("tasks", [])}

    cut_edges = report.get("cut_edges")
    expect(isinstance(cut_edges, list), f"report {report_path} is missing cut_edges")
    expect(report.get("cut_edge_count") == len(cut_edges), "cut_edge_count does not match cut_edges length")
    expect(len(cut_edges) > 0, "expected at least one cut edge")
    expect(report.get("uncut_batch_count", 0) > report.get("cut_batch_count", 0),
           "cut-aware planner did not reduce pure batch count")

    batch_calls = [
        (int(begin), int(end))
        for begin, end in re.findall(r"mtRunPureBatch\((\d+),\s*(\d+),\s*oldFlag\);", text)
    ]
    expect(batch_calls, "generated runtime has no mtRunPureBatch calls")

    for edge in cut_edges:
        from_id = edge.get("from_cpp_id")
        to_id = edge.get("to_cpp_id")
        expect(isinstance(from_id, int) and isinstance(to_id, int), f"invalid cut edge ids: {edge}")
        expect(from_id in tasks, f"cut edge source {from_id} missing from schedule {schedule_path}")
        expect(to_id in tasks, f"cut edge sink {to_id} missing from schedule {schedule_path}")
        expect(to_id in tasks[from_id].get("succ_cpp_ids", []),
               f"cut edge {from_id}->{to_id} does not exist in original succ_cpp_ids")
        expect(tasks[from_id].get("task_kind") == "pure_compute", f"cut source {from_id} is not pure_compute")
        expect(tasks[to_id].get("task_kind") == "pure_compute", f"cut sink {to_id} is not pure_compute")

        sink_report = report_tasks.get(to_id, {})
        expect(sink_report.get("selected") is True, f"cut sink {to_id} is not selected")
        expect(sink_report.get("runtime_applied") is True, f"cut sink {to_id} is not runtime_applied")
        expect(sink_report.get("cut_in_edges", 0) > 0, f"cut sink {to_id} has no cut_in_edges")
        expect(report_tasks.get(from_id, {}).get("cut_out_edges", 0) > 0,
               f"cut source {from_id} has no cut_out_edges")
        expect(any(begin <= from_id < end and begin <= to_id < end for begin, end in batch_calls),
               f"no generated batch covers cut edge {from_id}->{to_id}")

    print(f"mt-repcut-lite-cut ok: {len(cut_edges)} cut edges applied from {report_path}")


if __name__ == "__main__":
    main()
