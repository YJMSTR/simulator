#!/usr/bin/env python3
import argparse
import re
import sys
from pathlib import Path


def fail(message):
    print(f"mt-profile-off-fast-path failed: {message}", file=sys.stderr)
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


def extract_functions_matching(text, pattern):
    bodies = []
    for match in re.finditer(pattern, text):
        brace = text.find("{", match.end() - 1)
        if brace < 0:
            continue
        depth = 0
        for index in range(brace, len(text)):
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
                if depth == 0:
                    bodies.append(text[match.start():index + 1])
                    break
    return bodies


def extract_else_bodies_after_profile_check(body):
    results = []
    start = 0
    token = "if (mtProfileEnabled)"
    while True:
        if_pos = body.find(token, start)
        if if_pos < 0:
            break
        brace = body.find("{", if_pos)
        if brace < 0:
            break
        depth = 0
        close = -1
        for index in range(brace, len(body)):
            if body[index] == "{":
                depth += 1
            elif body[index] == "}":
                depth -= 1
                if depth == 0:
                    close = index
                    break
        if close < 0:
            break
        tail = body[close + 1:]
        else_match = re.match(r"\s*else\s*\{", tail)
        if else_match:
            else_brace = close + 1 + else_match.end() - 1
            depth = 0
            for index in range(else_brace, len(body)):
                if body[index] == "{":
                    depth += 1
                elif body[index] == "}":
                    depth -= 1
                    if depth == 0:
                        results.append(body[else_brace + 1:index])
                        start = index + 1
                        break
            else:
                break
        else:
            start = close + 1
    return results


def line_prefix(body, pos):
    return body[body.rfind("\n", 0, pos) + 1:pos]


def profile_block_ranges(body):
    ranges = []
    start = 0
    token = "if (mtProfileEnabled)"
    while True:
        if_pos = body.find(token, start)
        if if_pos < 0:
            return ranges
        brace = body.find("{", if_pos)
        semicolon = body.find(";", if_pos)
        if brace < 0 or (semicolon >= 0 and semicolon < brace):
            start = if_pos + len(token)
            continue
        depth = 0
        for index in range(brace, len(body)):
            if body[index] == "{":
                depth += 1
            elif body[index] == "}":
                depth -= 1
                if depth == 0:
                    ranges.append((brace, index))
                    start = index + 1
                    break
        else:
            return ranges


def is_profile_guarded(body, pos, ranges):
    if "if (mtProfileEnabled)" in line_prefix(body, pos):
        return True
    return any(start < pos < end for start, end in ranges)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model_dir")
    args = parser.parse_args()

    model_dir = source_dir(args.model_dir)
    text = read_sources(model_dir)
    substep = extract_function(text, "subStep0")
    batch = extract_function(text, "mtRunPureBatch")
    direct_shards = extract_functions_matching(text, r"\bvoid\s+\w+::mtRunPureBatchDirectShard\d+\s*\([^)]*\)\s*\{")
    expect(direct_shards, "generated runtime has no mtRunPureBatchDirectShard helpers")

    checked_bodies = [("subStep0", substep), ("mtRunPureBatch", batch)]
    checked_bodies.extend((f"mtRunPureBatchDirectShard{index}", body) for index, body in enumerate(direct_shards))
    for name, body in checked_bodies:
        ranges = profile_block_ranges(body)
        for match in re.finditer(r"recordMtProfileTask\(", body):
            expect(is_profile_guarded(body, match.start(), ranges),
                   f"{name} has recordMtProfileTask outside a profile-enabled branch")
        for match in re.finditer(r"std::chrono::steady_clock::now\(\)", body):
            expect(is_profile_guarded(body, match.start(), ranges),
                   f"{name} has steady_clock::now outside a profile-enabled branch")

    else_bodies = extract_else_bodies_after_profile_check(substep) + extract_else_bodies_after_profile_check(batch)
    for body in direct_shards:
        else_bodies.extend(extract_else_bodies_after_profile_check(body))
    direct_task_else = [body for body in else_bodies if re.search(r"\bmtTask\d+\([^,\n;]+?\);", body)]
    expect(direct_task_else, "no profile-off direct-active mtTask branch found")
    for body in direct_task_else:
        expect("recordMtProfileTask" not in body, "profile-off direct-active branch calls recordMtProfileTask")
        expect("std::chrono::steady_clock::now" not in body, "profile-off direct-active branch calls steady_clock::now")

    expect(re.search(r"else\s*\{\s*mtTask\d+\([^,\n;]+?\);\s*\}", substep),
           "subStep0 lacks plain profile-off direct-active mtTask branch")
    expect(any(re.search(r"else\s*\{\s*mtTask\d+\(activeWord\);\s*\}", body) for body in direct_shards),
           "mtRunPureBatch direct shard path lacks plain profile-off mtTask(activeWord) branch")

    print(f"mt-profile-off-fast-path ok: {model_dir}")


if __name__ == "__main__":
    main()
