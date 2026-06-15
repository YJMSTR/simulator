#!/usr/bin/env python3
import re
import sys
from pathlib import Path


def fail(message):
    print(f"mt-active-buffer-cost-reduction failed: {message}", file=sys.stderr)
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
        fail("usage: check-mt-active-buffer-cost-reduction.py <model-dir-or-gsim-compile-dir>")
    text = read_sources(source_dir(sys.argv[1]))

    if "struct ActivationDelta" in text:
        body = extract_function(text, "mtRunPureBatch")
        expect("mtWorkerDeltas" in text and "mtWorkerFlags" in text,
               "generated mt model missing persistent worker delta/flag members")
        expect("mtWorkerDeltas.resize" in body and "mtWorkerFlags.resize" in body,
               "mtRunPureBatch does not reuse persistent worker delta storage")
        expect("mtWorkerBuffers" not in body,
               "mtRunPureBatch still uses ActiveBuffer worker storage")
    else:
        for token in ("struct ActiveBuffer", "touchedWords", "touchedCount", "allActive"):
            expect(token in text, f"generated ActiveBuffer missing {token}")
        expect(re.search(r"for\s*\([^)]*touchedIdx[^)]*touchedCount[^)]*\)\s*words\[touchedWords\[touchedIdx\]\]\s*=\s*0", text),
               "ActiveBuffer::clear does not clear only touched words on normal path")
        expect(re.search(r"for\s*\([^)]*touchedIdx[^)]*touchedCount[^)]*\)[\s\S]{0,220}activeFlags\[wordIdx\]\s*\|=\s*words\[wordIdx\]", text),
               "ActiveBuffer::mergeFrom does not merge only touched words on normal path")
        expect("memset(words, 0xff, sizeof(words));" in text, "activateAll full-buffer slow path missing")
        body = extract_function(text, "mtRunPureBatch")
        expect("mtWorkerBuffers" in text and "mtWorkerFlags" in text,
               "generated model missing persistent worker buffer/flag members")
        expect("mtWorkerBuffers.resize" in body and "mtWorkerFlags.resize" in body,
               "mtRunPureBatch does not reuse persistent worker storage")
    expect("std::vector<ActiveBuffer> workerBuffers" not in body,
           "mtRunPureBatch still allocates fresh worker buffer vectors per batch")
    expect(re.search(r"std::vector<\s*uint\d+_t\s*>\s*workerFlags", body) is None,
           "mtRunPureBatch still allocates fresh worker flag vectors per batch")
    print(f"mt-active-buffer-cost-reduction ok: {source_dir(sys.argv[1])}")


if __name__ == "__main__":
    main()
