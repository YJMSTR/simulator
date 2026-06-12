#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message):
    print(f"mt-helper-equivalence check failed: {message}", file=sys.stderr)
    sys.exit(1)


def expect(condition, message):
    if not condition:
        fail(message)


def model_info(model_dir):
    headers = sorted(model_dir.glob("*.h"))
    expect(len(headers) == 1, f"expected exactly one generated header under {model_dir}, found {len(headers)}")
    header = headers[0]
    text = header.read_text()
    class_match = re.search(r"\bclass\s+S([A-Za-z_][A-Za-z0-9_]*)\s*\{", text)
    expect(class_match is not None, f"could not find generated model class in {header}")
    top = class_match.group(1)
    setters = re.findall(r"\bvoid\s+set_([A-Za-z_][A-Za-z0-9_]*)\(([^)]+?)\s+val\);", text)
    getters = re.findall(r"\b([A-Za-z0-9_:_BitInt<>\s]+?)\s+get_([A-Za-z_][A-Za-z0-9_]*)\(\);", text)
    expect(getters, f"no generated output getters found in {header}")
    sources = sorted(model_dir.glob("*.cpp"))
    expect(sources, f"no generated C++ sources found under {model_dir}")
    return top, setters, getters, header, sources


def driver_source(top, setters, getters, header_name):
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
        "  if (lower == \"clock\" || (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, \"clock\") == 0)) return cycle & 1;",
        "  if (lower == \"reset\" || (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, \"reset\") == 0)) return cycle < 2 || cycle == 7;",
        "  if (lower.find(\"start\") != std::string::npos) return cycle == 2 || cycle == 5 || cycle == 8;",
        "  if (lower.find(\"gate\") != std::string::npos) return (cycle % 3) != 0;",
        "  return (uint64_t)((cycle + 3) * (index + 5));",
        "}",
        "",
        "int main() {",
        f"  S{top} dut;",
        "  for (int cycle = 0; cycle < 16; cycle ++) {",
    ]
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


def build_and_run(model_dir, work_dir, label):
    top, setters, getters, header, sources = model_info(model_dir)
    driver = work_dir / f"{label}_driver.cpp"
    binary = work_dir / f"{label}_driver"
    driver.write_text(driver_source(top, setters, getters, header.name))
    compiler = os.environ.get("CXX", "clang++")
    command = [
        compiler,
        "-std=c++17",
        "-O0",
        "-I",
        str(model_dir),
        str(driver),
        *[str(source) for source in sources],
        "-o",
        str(binary),
    ]
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
    return result.stdout


def main():
    if len(sys.argv) != 3:
        fail("usage: check-mt-helper-equivalence.py <helper-seq-dir> <buffered-seq-dir>")

    helper_dir = Path(sys.argv[1])
    buffered_dir = Path(sys.argv[2])
    expect(helper_dir.is_dir(), f"{helper_dir} does not exist")
    expect(buffered_dir.is_dir(), f"{buffered_dir} does not exist")
    expect(shutil.which(os.environ.get("CXX", "clang++")) is not None, "C++ compiler not found")

    with tempfile.TemporaryDirectory(prefix="gsim-mt-equivalence-") as tmp:
        work_dir = Path(tmp)
        helper_trace = build_and_run(helper_dir, work_dir, "helper")
        buffered_trace = build_and_run(buffered_dir, work_dir, "buffered")

    if helper_trace != buffered_trace:
        print("helper-seq trace:", file=sys.stderr)
        print(helper_trace, file=sys.stderr)
        print("buffered-seq trace:", file=sys.stderr)
        print(buffered_trace, file=sys.stderr)
        fail("generated model traces differ")

    print(f"helper-seq and buffered-seq traces match for {buffered_dir}")


if __name__ == "__main__":
    main()
