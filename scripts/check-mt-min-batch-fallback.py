#!/usr/bin/env python3
import argparse
import re
import sys
from pathlib import Path


def fail(message):
    print(f"mt-min-batch-fallback failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


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
    text = generated_text(model_dir)

    expect("mtRunPureBatch" in text, "generated runtime has no mtRunPureBatch")
    expect("GSIM_MT_MIN_BATCH_TASKS" in text, "missing GSIM_MT_MIN_BATCH_TASKS environment hook")
    expect(re.search(r"\bint\s+mtMinBatchTasks\s*=\s*16\s*;", text),
           "missing default mtMinBatchTasks = 16")
    expect(re.search(r"minBatchEnv\s*!=\s*nullptr", text),
           "missing min-batch environment parsing")
    expect(re.search(r"taskCount\s*<\s*mtMinBatchTasks", text),
           "missing taskCount < mtMinBatchTasks guard")
    expect(re.search(r"taskCount\s*<\s*mtMinBatchTasks[\s\S]{0,160}workerCount\s*=\s*1\s*;", text),
           "min-batch guard does not force workerCount = 1")

    print(f"mt-min-batch-fallback ok: {model_dir}")


if __name__ == "__main__":
    main()
