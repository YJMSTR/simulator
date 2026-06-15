#!/usr/bin/env python3
import re
import sys
from pathlib import Path


def fail(message):
    print(f"mt-single-worker-active-bypass failed: {message}", file=sys.stderr)
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


def main():
    if len(sys.argv) != 2:
        fail("usage: check-mt-single-worker-active-bypass.py <model-dir-or-gsim-compile-dir>")
    text = read_sources(source_dir(sys.argv[1]))

    expect(re.search(r"\bvoid\s+\w+::mtTask\d+\s*\(\s*uint\d+_t\s*&\s*flag\s*\)", text),
           "missing direct-active mtTask overload")
    expect(re.search(r"\bvoid\s+\w+::mtTask\d+\s*\(\s*uint\d+_t\s*&\s*flag\s*,\s*(?:ActiveBuffer|ActivationDelta)\s*&\s*nextActive\s*\)", text),
           "missing generic buffered/delta mtTask overload")

    body = extract_function(text, "mtRunPureBatch")
    expect("workerCount == 1" in body, "missing workerCount == 1 branch")
    expect("mtTask" in body and "(activeWord)" in body, "single-worker branch does not call direct-active mtTask")

    branch_match = re.search(
        r"if\s*\(\s*workerCount\s*==\s*1\s*\)\s*\{([\s\S]*?)\n\s*\}\n\s*std::vector<uint64_t>",
        body,
    )
    expect(branch_match is not None, "single-worker branch must complete before generic worker/profile setup")
    single_worker_body = branch_match.group(1)
    for forbidden in ("ActiveBuffer", "ActivationDelta", ".clear(", ".mergeFrom(", ".mergeInto(", ".orWord(", "mtWorkerBuffers", "mtWorkerDeltas"):
        expect(forbidden not in single_worker_body, f"single-worker branch uses {forbidden}")
    expect("activeFlags[" in single_worker_body or "activeWord" in single_worker_body,
           "single-worker branch lacks direct active flag/word use")
    expect("return;" in single_worker_body, "single-worker branch should return before generic buffered path")

    expect("mtWorkerDeltas" in body or "std::vector<ActiveBuffer>" in body,
           "generic multi-worker path missing activation storage")
    expect("std::vector<std::thread>" in body, "generic multi-worker branch missing std::thread vector")
    print(f"mt-single-worker-active-bypass ok: {source_dir(sys.argv[1])}")


if __name__ == "__main__":
    main()
