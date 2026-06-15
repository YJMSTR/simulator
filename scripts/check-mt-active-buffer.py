#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import sys
import tempfile
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


def model_header(model_dir):
    headers = sorted(model_dir.glob("*.h"))
    expect(headers, f"no generated header found under {model_dir}")
    expect(len(headers) == 1, f"expected one generated header under {model_dir}, found {len(headers)}")
    return headers[0]


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


def active_word_info(text):
    active_flags_match = re.search(r"\buint(\d+)_t\s+activeFlags\[(\d+)\];", text)
    expect(active_flags_match is not None, "activeFlags declaration not found")
    return int(active_flags_match.group(1)), int(active_flags_match.group(2))


def compiler():
    requested = os.environ.get("CXX")
    if requested:
        expect(shutil.which(requested) is not None, f"CXX compiler not found: {requested}")
        return requested
    for candidate in ("clang++", "clang++-19"):
        if shutil.which(candidate):
            return candidate
    fail("clang++ compiler not found")


def semantic_driver(header_name, width, active_words):
    return f"""\
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "{header_name}"

static int failures = 0;
using ActiveWord = uint{width}_t;
constexpr int activeWords = {active_words};
constexpr int packedWords = 64 / {width};
constexpr int paddedWords = activeWords + packedWords + 1;

static void initPattern(ActiveWord *flags, int count = activeWords) {{
  for (int i = 0; i < count; i ++) {{
    flags[i] = (ActiveWord)((i * 37) ^ 0x5a);
  }}
}}

static void referencePackedOr(ActiveWord *flags, int idx, uint64_t mask) {{
  for (int i = 0; i < packedWords && idx + i < activeWords; i ++) {{
    flags[idx + i] |= (ActiveWord)(mask >> (i * {width}));
  }}
}}

static void referenceLegacyOr(ActiveWord *flags, int idx, uint64_t mask) {{
  if (mask <= UINT64_C(0xff)) {{
    flags[idx] |= (ActiveWord)mask;
    return;
  }}

  int byteCount = 8;
  if (mask <= UINT64_C(0xffff)) byteCount = 2;
  else if (mask <= UINT64_C(0xffffffff)) byteCount = 4;

  uint8_t *bytes = reinterpret_cast<uint8_t *>(&flags[idx]);
  for (int byte = 0; byte < byteCount; byte ++) {{
    bytes[byte] |= (uint8_t)(mask >> (byte * 8));
  }}
}}

static void compareFlags(const ActiveWord *got, const ActiveWord *want, const char *label) {{
  for (int i = 0; i < activeWords; i ++) {{
    if (got[i] != want[i]) {{
      std::fprintf(stderr, "%s word %d: got 0x%llx want 0x%llx\\n",
                   label,
                   i,
                   (unsigned long long)got[i],
                   (unsigned long long)want[i]);
      failures ++;
    }}
  }}
}}

static void runSingleCase(int idx, uint64_t mask, const char *label) {{
  ActiveWord got[paddedWords];
  ActiveWord want[paddedWords];
  ActiveWord legacy[paddedWords];
  initPattern(got, paddedWords);
  initPattern(want, paddedWords);
  initPattern(legacy, paddedWords);
  ActiveBuffer buffer;
  buffer.orWord(idx, mask);
  buffer.mergeFrom(got);
  referencePackedOr(want, idx, mask);
  referenceLegacyOr(legacy, idx, mask);
  compareFlags(got, want, label);
  compareFlags(got, legacy, label);
}}

int main() {{
  runSingleCase(0, UINT64_C(0x0), "zero mask");
  runSingleCase(0, UINT64_C(0x1), "low bit");
  runSingleCase(3, UINT64_C(0x80), "high bit in one word");
  runSingleCase(0, UINT64_C(0x0201), "adjacent packed words");
  runSingleCase(1, UINT64_C(0x0000050300), "sparse shifted packed words");
  runSingleCase(0, UINT64_C(0x0400000000000201), "sparse packed words");
  runSingleCase(activeWords - 1, UINT64_C(0xffffffffffffffff), "bounded final word");
  if (activeWords >= 2) {{
    runSingleCase(activeWords - 2, UINT64_C(0xffffffffffffffff), "bounded final two words");
  }}
  if (activeWords > packedWords) {{
    runSingleCase(activeWords - packedWords, UINT64_C(0xffffffffffffffff), "full final packed group");
  }}

  ActiveWord got[activeWords];
  ActiveWord want[activeWords];
  initPattern(got);
  initPattern(want);
  ActiveBuffer buffer;
  buffer.orWord(0, UINT64_C(0x1));
  referencePackedOr(want, 0, UINT64_C(0x1));
  buffer.orWord(0, UINT64_C(0x200));
  referencePackedOr(want, 0, UINT64_C(0x200));
  buffer.orWord(activeWords - 1, UINT64_C(0xffffffffffffffff));
  referencePackedOr(want, activeWords - 1, UINT64_C(0xffffffffffffffff));
  buffer.mergeFrom(got);
  compareFlags(got, want, "repeated packed sequence");

  std::memset(got, 0, sizeof(got));
  buffer.clear();
  buffer.activateAll();
  buffer.mergeFrom(got);
  for (int i = 0; i < activeWords; i ++) {{
    if (got[i] != (ActiveWord)-1) {{
      std::fprintf(stderr, "activateAll word %d: got 0x%llx\\n",
                   i,
                   (unsigned long long)got[i]);
      failures ++;
      break;
    }}
  }}
  return failures == 0 ? 0 : 1;
}}
"""


def run_semantic_check(model_dir, text):
    header = model_header(model_dir)
    width, active_words = active_word_info(text)
    expect(active_words > 0, "active word count must be positive")
    cxx = compiler()
    with tempfile.TemporaryDirectory(prefix="gsim-mt-active-buffer-") as tmp:
        work_dir = Path(tmp)
        driver = work_dir / "active_buffer_driver.cpp"
        binary = work_dir / "active_buffer_driver"
        driver.write_text(semantic_driver(header.name, width, active_words))
        command = [
            cxx,
            "-std=c++17",
            "-O0",
            "-I",
            str(model_dir),
            str(driver),
            "-o",
            str(binary),
        ]
        compile_result = subprocess.run(command, text=True, capture_output=True)
        if compile_result.returncode != 0:
            sys.stderr.write(compile_result.stdout)
            sys.stderr.write(compile_result.stderr)
            fail("failed to compile ActiveBuffer::orWord semantic driver")
        run_result = subprocess.run([str(binary)], text=True, capture_output=True)
        if run_result.returncode != 0:
            sys.stderr.write(run_result.stdout)
            sys.stderr.write(run_result.stderr)
            fail("ActiveBuffer::orWord semantic driver failed")


def literal_mask(expr):
    expr = expr.strip()
    match = re.fullmatch(r"UINT64_C\(\s*(0x[0-9a-fA-F]+|\d+)\s*\)", expr)
    if match:
        return int(match.group(1), 0)
    match = re.fullmatch(r"0x[0-9a-fA-F]+|\d+", expr)
    if match:
        return int(expr, 0)
    return None


def conditional_packed_mask(expr):
    match = re.fullmatch(
        r"-\(\s*uint(?:8|16|32|64)_t\s*\)\s*[^&]+?\s*&\s*"
        r"(UINT64_C\(\s*(?:0x[0-9a-fA-F]+|\d+)\s*\)|0x[0-9a-fA-F]+|\d+)",
        expr.strip(),
    )
    if not match:
        return None
    return literal_mask(match.group(1))


def expected_ids_for_mask(idx, mask, width, active_words):
    return {
        (idx + bit // width) * width + (bit % width)
        for bit in range(64)
        if (mask & (1 << bit)) and idx + bit // width < active_words
    }


def check_orword_call_comments(text, width, active_words):
    literal_checks = 0
    conditional_checks = 0
    for match in re.finditer(r"\bnextActive\.orWord\(\s*(\d+)\s*,\s*([^;\n]+)\);\s*//\s*([^\n]*)", text):
        expr = match.group(2)
        mask = literal_mask(expr)
        is_conditional = False
        if mask is None:
            mask = conditional_packed_mask(expr)
            is_conditional = mask is not None
        if mask is None:
            continue
        idx = int(match.group(1))
        comment_ids = {int(value) for value in re.findall(r"\d+", match.group(3))}
        expected_ids = expected_ids_for_mask(idx, mask, width, active_words)
        expect(comment_ids == expected_ids,
               "orWord call/comment mismatch: "
               f"idx={idx} expr={expr.strip()} mask=0x{mask:x} comment={sorted(comment_ids)} "
               f"expected={sorted(expected_ids)}")
        if is_conditional:
            conditional_checks += 1
        else:
            literal_checks += 1
    return literal_checks, conditional_checks


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
    run_semantic_check(buffered_dir, buffered_text)
    width, active_words = active_word_info(buffered_text)
    literal_comment_checks, conditional_comment_checks = check_orword_call_comments(buffered_text, width, active_words)

    print(
        f"checked {len(buffered_tasks)} buffered mtTask helpers in {buffered_dir} "
        f"(literal_comment_checks={literal_comment_checks}, "
        f"conditional_comment_checks={conditional_comment_checks})"
    )


if __name__ == "__main__":
    main()
