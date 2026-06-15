#!/usr/bin/env python3
import argparse
import re
import sys
from pathlib import Path


def fail(message):
    print(f"mt-12x-mixed-mode failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def source_dir(path):
    candidate = Path(path)
    if (candidate / "model").is_dir():
        candidate = candidate / "model"
    expect(candidate.is_dir(), f"{candidate} is not a directory")
    return candidate


def read_sources(model_dir):
    paths = sorted(model_dir.glob("*.h")) + sorted(model_dir.glob("*.cpp"))
    expect(paths, f"no generated C++ sources found under {model_dir}")
    return "\n".join(path.read_text() for path in paths)


def extract_function(text, name):
    match = re.search(rf"\bvoid\s+\w+::{re.escape(name)}\s*\([^)]*\)\s*\{{", text)
    expect(match is not None, f"missing function {name}")
    brace = text.find("{", match.end() - 1)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[match.start():index + 1]
    fail(f"unterminated function {name}")


def first_branch_body(body, condition):
    token = f"if ({condition})"
    start = body.find(token)
    expect(start >= 0, f"missing branch: {token}")
    brace = body.find("{", start)
    expect(brace >= 0, f"missing branch body for {token}")
    depth = 0
    for index in range(brace, len(body)):
        if body[index] == "{":
            depth += 1
        elif body[index] == "}":
            depth -= 1
            if depth == 0:
                return body[brace + 1:index]
    fail(f"unterminated branch for {token}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model_dir")
    args = parser.parse_args()

    model_dir = source_dir(args.model_dir)
    text = read_sources(model_dir)
    body = extract_function(text, "mtRunPureBatch")

    for token in (
        "struct ActivationDelta",
        "struct ActivationDeltaEntry",
        "std::vector<ActivationDelta> mtWorkerDeltas",
        "mtProfileTrueParallelBatchCount",
        "mtProfileSkippedFakeParallelBatchCount",
        "mtProfileActivationDeltaEntries",
        "mtProfileEffectiveWorkerCountHist",
        "mtProfileBatchSizeHist",
        "mtProfileRejectBelowMinBatch",
    ):
        expect(token in text, f"missing {token}")

    expect("std::vector<ActiveBuffer> mtWorkerBuffers" not in text,
           "normal mt helper mode should not keep persistent ActiveBuffer worker storage")
    expect("mtWorkerDeltas.resize" in body, "true parallel path does not reuse persistent ActivationDelta storage")
    expect(".mergeInto(activeFlags)" in body, "true parallel path does not merge ActivationDelta into activeFlags")
    expect("mtWorkerBuffers" not in body, "mtRunPureBatch still uses ActiveBuffer worker storage")
    expect(".mergeFrom(activeFlags)" not in body, "mtRunPureBatch still merges ActiveBuffer")

    serial_branch = first_branch_body(body, "workerCount == 1")
    for forbidden in ("ActivationDelta", "ActiveBuffer", "mtWorkerDeltas", ".clear()", ".mergeInto(", ".mergeFrom("):
        expect(forbidden not in serial_branch, f"workerCount == 1 branch uses {forbidden}")
    expect(re.search(r"\bmtTask\d+\(activeWord\);", serial_branch),
           "workerCount == 1 branch does not call direct-active mtTask")
    expect("mtProfileSkippedFakeParallelBatchCount ++" in serial_branch,
           "workerCount == 1 branch does not record skipped fake-parallel batches")
    expect("recordMtProfileTask" in serial_branch and "mtProfileSerialFastTaskCount ++" in text,
           "workerCount == 1 branch does not record serial-fast task count")

    expect(re.search(r"if\s*\(taskCount\s*<\s*mtMinBatchTasks\)\s*\{[\s\S]*?mtProfileRejectBelowMinBatch\s*\+\+;[\s\S]*?workerCount\s*=\s*1;", body),
           "below-threshold batches are not rejected and counted before serial fast path")
    expect(re.search(r"if\s*\(workerCount\s*<\s*2\)\s*workerCount\s*=\s*1\s*;", body),
           "effective worker count is not clamped away from fake parallelism")
    expect("mtProfileTrueParallelBatchCount ++;" in body,
           "true parallel batches are not counted separately")

    substep = extract_function(text, "subStep0")
    expect("if (mtConfiguredWorkerCount == 1)" not in substep,
           "non-batch mt tasks still branch on configured thread count")
    expect("ActiveBuffer mtBuffer;" not in substep,
           "non-batch mt tasks still instantiate ActiveBuffer")
    expect(re.search(r"\bmtTask\d+\([^,\n]+?\);", substep),
           "subStep0 lacks direct-active mtTask calls for non-batch tasks")

    print(f"mt-12x-mixed-mode ok: {model_dir}")


if __name__ == "__main__":
    main()
