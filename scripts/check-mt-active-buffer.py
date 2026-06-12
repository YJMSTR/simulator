#!/usr/bin/env python3
import re
import sys
from pathlib import Path


def fail(message):
    print(f"mt-active-buffer check failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def read_sources(model_dir):
    paths = sorted(model_dir.glob("*.h")) + sorted(model_dir.glob("*.cpp"))
    expect(paths, f"no generated C++ sources found under {model_dir}")
    return "\n".join(path.read_text() for path in paths)


def extract_function(text, name):
    match = re.search(rf"\bvoid\s+\w+::({re.escape(name)})\s*\([^)]*\)\s*\{{", text)
    expect(match is not None, f"missing function {name}")
    start = match.start()
    brace = text.find("{", match.end() - 1)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    fail(f"unterminated function {name}")


def main():
    if len(sys.argv) != 3:
        fail("usage: check-mt-active-buffer.py <helper-seq-dir> <buffered-seq-dir>")

    helper_dir = Path(sys.argv[1])
    buffered_dir = Path(sys.argv[2])
    expect(helper_dir.is_dir(), f"{helper_dir} does not exist")
    expect(buffered_dir.is_dir(), f"{buffered_dir} does not exist")

    helper_text = read_sources(helper_dir)
    buffered_text = read_sources(buffered_dir)

    for token in ("struct ActiveBuffer", "void clear()", "void orWord(", "void mergeFrom("):
        expect(token in buffered_text, f"buffered-seq output missing {token}")

    helper_tasks = sorted(set(re.findall(r"\bvoid\s+\w+::(mtTask\d+)\s*\(", helper_text)))
    buffered_tasks = sorted(set(re.findall(r"\bvoid\s+\w+::(mtTask\d+)\s*\(", buffered_text)))
    expect(helper_tasks, "helper-seq output has no mtTask helpers")
    expect(helper_tasks == buffered_tasks, "buffered-seq helper task set differs from helper-seq")

    for task in buffered_tasks:
        body = extract_function(buffered_text, task)
        expect("ActiveBuffer" in body, f"{task} does not use ActiveBuffer")
        expect("activeFlags[" not in body, f"{task} writes or reads global activeFlags directly")
        expect("activateAll()" not in body, f"{task} calls activateAll directly instead of serial handling")
        expect(not re.search(r"\bsubReset\d+\(\);", body),
               f"{task} calls a serial reset helper directly")

    merge_calls = re.findall(r"\bmtBuffer\.mergeFrom\(", buffered_text)
    expect(merge_calls, "buffered-seq driver never merges task buffers")
    expect(not re.search(r"\bmtTask\d+\(activeFlags\[", buffered_text),
           "buffered-seq driver passes global activeFlags directly to a helper")

    print(f"checked {len(buffered_tasks)} buffered mtTask helpers in {buffered_dir}")


if __name__ == "__main__":
    main()
