#!/usr/bin/env python3
import re
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message):
    print(f"mt-activation-delta-orword failed: {message}", file=sys.stderr)
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


def model_header(model_dir):
    headers = sorted(model_dir.glob("*.h"))
    expect(headers, f"no generated header found under {model_dir}")
    expect(len(headers) == 1, f"expected one generated header under {model_dir}, found {len(headers)}")
    return headers[0]


def extract_struct(text, struct_name):
    struct_match = re.search(rf"\bstruct\s+{re.escape(struct_name)}\s*\{{", text)
    expect(struct_match is not None, f"missing struct {struct_name}")
    brace = text.find("{", struct_match.start())
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[struct_match.start():index + 1]
    fail(f"unterminated struct {struct_name}")


def extract_method(text, struct_name, method_name):
    struct_body = extract_struct(text, struct_name)
    method_match = re.search(rf"\bvoid\s+{re.escape(method_name)}\s*\([^)]*\)\s*(?:const\s*)?\{{", struct_body)
    expect(method_match is not None, f"missing {struct_name}::{method_name}")
    brace = struct_body.find("{", method_match.end() - 1)
    depth = 0
    for index in range(brace, len(struct_body)):
        if struct_body[index] == "{":
            depth += 1
        elif struct_body[index] == "}":
            depth -= 1
            if depth == 0:
                return struct_body[method_match.start():index + 1]
    fail(f"unterminated {struct_name}::{method_name}")


def active_word_info(text):
    width_match = re.search(r"\bvoid\s+mergeInto\s*\(\s*uint(\d+)_t\s*\*\s*activeFlags\s*\)", text)
    expect(width_match is not None, "ActivationDelta::mergeInto active word type not found")
    width = int(width_match.group(1))
    active_flags_match = re.search(rf"\buint{width}_t\s+activeFlags\[(\d+)\];", text)
    if active_flags_match is not None:
        return width, int(active_flags_match.group(1))

    merge_body = extract_method(text, "ActivationDelta", "mergeInto")
    loop_match = re.search(r"for\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*(\d+)\s*;", merge_body)
    expect(loop_match is not None, "could not infer active word count")
    return width, int(loop_match.group(1))


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

static void expectWord(uint64_t got, uint64_t want, const char *label) {{
  if (got != want) {{
    std::fprintf(stderr, "%s: got 0x%llx want 0x%llx\\n",
                 label,
                 (unsigned long long)got,
                 (unsigned long long)want);
    failures ++;
  }}
}}

static void initPattern(ActiveWord *flags, int count = activeWords) {{
  for (int i = 0; i < count; i ++) {{
    flags[i] = (ActiveWord)((i * 37) ^ 0x5a);
  }}
}}

static void clearFlags(ActiveWord *flags) {{
  std::memset(flags, 0, sizeof(ActiveWord) * activeWords);
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
  ActivationDelta delta;
  delta.orWord(idx, mask);
  delta.mergeInto(got);
  referencePackedOr(want, idx, mask);
  referenceLegacyOr(legacy, idx, mask);
  compareFlags(got, want, label);
  compareFlags(got, legacy, label);
}}

int main() {{
  ActiveWord flags[activeWords];
  ActiveWord want[activeWords];

  ActivationDelta delta;
  clearFlags(flags);
  delta.orWord(0, UINT64_C(0x1));
  delta.orWord(0, UINT64_C(0x2));
  delta.mergeInto(flags);
  expectWord(flags[0], 0x3, "repeated entries OR into word 0");

  if (activeWords >= 2) {{
    clearFlags(flags);
    delta.clear();
    delta.orWord(0, UINT64_C(0x0201));
    delta.mergeInto(flags);
    expectWord(flags[0], 0x1, "packed byte 0 maps to idx");
    expectWord(flags[1], 0x2, "packed byte 1 maps to idx + 1");
  }}

  if (activeWords >= 4) {{
    clearFlags(flags);
    delta.clear();
    flags[2] = (ActiveWord)0x80;
    delta.orWord(1, UINT64_C(0x0000050300));
    delta.mergeInto(flags);
    expectWord(flags[1], 0x0, "zero packed byte is skipped");
    expectWord(flags[2], 0x83, "nonzero base idx preserves existing bits");
    expectWord(flags[3], 0x5, "nonzero base idx maps subsequent packed byte");
  }}

  if (activeWords >= 8) {{
    clearFlags(flags);
    delta.clear();
    delta.orWord(0, UINT64_C(0x0400000000000201));
    delta.mergeInto(flags);
    expectWord(flags[0], 0x1, "packed low byte");
    expectWord(flags[1], 0x2, "packed second byte");
    expectWord(flags[7], 0x4, "packed high byte");
  }}

  runSingleCase(0, UINT64_C(0x0), "reference zero mask");
  runSingleCase(0, UINT64_C(0x1), "reference low bit");
  runSingleCase(3, UINT64_C(0x80), "reference high bit in one word");
  runSingleCase(0, UINT64_C(0x0201), "reference adjacent packed words");
  runSingleCase(1, UINT64_C(0x0000050300), "reference sparse shifted packed words");
  runSingleCase(0, UINT64_C(0x0400000000000201), "reference sparse packed words");
  runSingleCase(activeWords - 1, UINT64_C(0xffffffffffffffff), "reference bounded final word");
  if (activeWords >= 2) {{
    runSingleCase(activeWords - 2, UINT64_C(0xffffffffffffffff), "reference bounded final two words");
  }}
  if (activeWords > packedWords) {{
    runSingleCase(activeWords - packedWords, UINT64_C(0xffffffffffffffff), "reference full final packed group");
  }}

  initPattern(flags);
  initPattern(want);
  delta.clear();
  delta.orWord(0, UINT64_C(0x1));
  referencePackedOr(want, 0, UINT64_C(0x1));
  delta.orWord(0, UINT64_C(0x200));
  referencePackedOr(want, 0, UINT64_C(0x200));
  delta.orWord(activeWords - 1, UINT64_C(0xffffffffffffffff));
  referencePackedOr(want, activeWords - 1, UINT64_C(0xffffffffffffffff));
  delta.mergeInto(flags);
  compareFlags(flags, want, "reference repeated packed sequence");

  clearFlags(flags);
  delta.clear();
  delta.activateAll();
  delta.mergeInto(flags);
  for (int i = 0; i < activeWords; i ++) {{
    if (flags[i] != (ActiveWord)-1) {{
      std::fprintf(stderr, "activateAll word %d: got 0x%llx\\n",
                   i,
                   (unsigned long long)flags[i]);
      failures ++;
      break;
    }}
  }}

  return failures == 0 ? 0 : 1;
}}
"""


def run_semantic_check(model_dir, header, width, active_words):
    expect(active_words > 0, "active word count must be positive")
    cxx = compiler()
    with tempfile.TemporaryDirectory(prefix="gsim-mt-orword-") as tmp:
        work_dir = Path(tmp)
        driver = work_dir / "activation_delta_orword_driver.cpp"
        binary = work_dir / "activation_delta_orword_driver"
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
            fail("failed to compile ActivationDelta::orWord semantic driver")
        run_result = subprocess.run([str(binary)], text=True, capture_output=True)
        if run_result.returncode != 0:
            sys.stderr.write(run_result.stdout)
            sys.stderr.write(run_result.stderr)
            fail("ActivationDelta::orWord semantic driver failed")


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


def call_stats(text, width):
    calls = 0
    packed_calls = 0
    for match in re.finditer(r"\bnextActive\.orWord\(\s*(\d+)\s*,\s*([^;\n]+)\);\s*//\s*([^\n]*)", text):
        calls += 1
        ids = [int(value) for value in re.findall(r"\d+", match.group(3))]
        words = {value // width for value in ids}
        if len(words) > 1:
            packed_calls += 1
    return calls, packed_calls


def main():
    if len(sys.argv) != 2:
        fail("usage: check-mt-activation-delta-orword.py <model-dir-or-gsim-compile-dir>")

    model_dir = source_dir(sys.argv[1])
    text = read_sources(model_dir)
    header = model_header(model_dir)
    expect("struct ActivationDelta" in text, "generated model has no ActivationDelta")

    body = extract_method(text, "ActivationDelta", "orWord")
    expect(re.search(r"for\s*\([^)]*idx\s*\+\s*i\s*<", body),
           "ActivationDelta::orWord does not iterate over packed active words")
    expect(re.search(r"mask\s*>>\s*\(\s*i\s*\*\s*\d+\s*\)", body),
           "ActivationDelta::orWord does not unpack mask by ACTIVE_WIDTH chunks")
    expect("entries.push_back({wordIdx, value})" in body,
           "ActivationDelta::orWord does not append unpacked word entries")
    expect("entry.idx" in extract_method(text, "ActivationDelta", "mergeInto"),
           "ActivationDelta::mergeInto does not merge by entry index")
    width, active_words = active_word_info(text)
    run_semantic_check(model_dir, header, width, active_words)
    literal_comment_checks, conditional_comment_checks = check_orword_call_comments(text, width, active_words)
    calls, packed_calls = call_stats(text, width)

    print(
        f"mt-activation-delta-orword ok: {model_dir} "
        f"({active_words} uint{width}_t active words, orWord calls={calls}, "
        f"packed_calls={packed_calls}, literal_comment_checks={literal_comment_checks}, "
        f"conditional_comment_checks={conditional_comment_checks})"
    )


if __name__ == "__main__":
    main()
