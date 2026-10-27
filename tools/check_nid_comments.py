#!/usr/bin/env python3
"""Keep the `/* NID: 0x... */` annotations in the HLE sources honest.

A NID annotation is evidence: it says "this function answers the guest import
0x...". Registration never reads it -- tools/gen_hle_nids.py computes every NID
from the function name (tools/nid_database.compute_nid) -- so a stale comment
is invisible to the build and wrong to everyone who reads it. In cellGcmSys 33
of 36 were wrong; most held real NIDs of *other* functions, because the list
behind them had been permuted.

This checker computes the NID from the name of the function the comment
documents and reports every comment that disagrees, every comment that
documents nothing, and every comment whose value cannot be read.

    python3 tools/check_nid_comments.py                  # check libs/
    python3 tools/check_nid_comments.py libs include     # check paths
    python3 tools/check_nid_comments.py --fix libs       # rewrite mismatches
    python3 tools/check_nid_comments.py --json libs      # machine-readable

Binding.  A comment must be the last thing before the function it documents:
nothing but whitespace may come between them. Anything else is a finding,
because then the annotated function is *not* the one that immediately follows
-- which is exactly how a permuted list reproduced itself. So a trailing
comment (code before it on its line), another comment, a preprocessor
directive, a struct, a macro, or any other construct makes the comment an
orphan. The annotated function is parsed from the declaration itself, never
from the comment, so the check proves the pairing instead of trusting it.

In an implementation file (.c/.cpp/.m/.mm) the comment must sit on a
definition: a NID belongs to the body that registration actually generates a
handler for, and a forward declaration between the comment and that body would
otherwise get a freshly computed, plausible-looking value of its own (a
`static u32 helper(u32);` prototype is not an export). A header has no body to
attach to, so a declaration is checked in place -- same immediate-follows rule,
same computed value.

Only a comment whose *body starts* with `NID` is an annotation; a NID quoted
in the middle of a prose comment is prose. The value is 0x and eight hex
digits; anything else in that position is `malformed`.

`--fix` rewrites mismatch values in place and preserves the comment's exact
form (spacing, punctuation, case, trailing prose). Orphans and malformed
comments are never rewritten: there is no right value to write for them.
`--fix` is idempotent -- a second run changes nothing and exits 0.

Exit status: 0 clean, 1 findings, 2 usage or I/O error.
"""

import argparse
import bisect
import json
import os
import re
import sys
from dataclasses import dataclass
from typing import Iterator, List, Optional, Tuple

# The one NID authority in this tree; never reimplement the hash here.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nid_database import compute_nid  # noqa: E402

SCHEMA = "nid-comments/1"
DEFAULT_PATHS = ("libs",)
SCANNED_EXTENSIONS = (".c", ".cpp", ".h", ".m", ".mm")
IMPLEMENTATION_EXTENSIONS = (".c", ".cpp", ".m", ".mm")

# A NID value: 0x and exactly eight hex digits, on a word boundary (a ninth
# digit means the value is not a 32-bit NID).
NID_VALUE = re.compile(r"0[xX]([0-9A-Fa-f]{8})\b")

# The comment body starts with NID, an optional colon, then the value. The
# decoration prefix allows `/**` blocks whose lines start with `*`.
ANNOTATION_HEAD = re.compile(r"\s*(?:\*\s*)*NID\b\s*:?[ \t]*")

IDENT = re.compile(r"[A-Za-z_]\w*")

# Words that can never be the name of a function declarator.
KEYWORDS = frozenset("""
    alignof asm auto bool break case char const constexpr continue default do
    double else enum extern float for goto if inline int long register return
    short signed sizeof static struct switch typedef union unsigned void
    volatile while _Bool _Complex
""".split())


class CheckError(Exception):
    """A usage or I/O problem: exit status 2."""


def hex32(value: Optional[int]) -> Optional[str]:
    return None if value is None else f"0x{value:08X}"


# ---------------------------------------------------------------------------
# Lexing: find the block comments and hand back code with them blanked out
# ---------------------------------------------------------------------------

@dataclass
class Comment:
    start: int          # offset of the opening slash
    end: int            # offset just past the closing slash of `*/`


def lex(text: str) -> Tuple[List[Comment], str]:
    """Return the block comments and *text* with every comment blanked to spaces.

    Blanking keeps offsets and line numbers intact, so the declaration that
    follows a comment can be scanned without the comment text (or a `//` in
    prose) taking part in the parse.  String and character literals are
    skipped so a `/*` inside one is not mistaken for a comment.
    """
    comments: List[Comment] = []
    out = list(text)
    n = len(text)
    i = 0
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            end = n if j < 0 else j + 2
            comments.append(Comment(i, end))
            for k in range(i, end):
                if out[k] != "\n":
                    out[k] = " "
            i = end
        elif c in "\"'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                if text[i] == "\n":       # unterminated literal: stop at EOL
                    break
                i += 1
        else:
            i += 1
    return comments, "".join(out)


# ---------------------------------------------------------------------------
# Code scanning: whitespace, directives, balanced brackets, declarators
# ---------------------------------------------------------------------------

def skip_ws(code: str, i: int) -> int:
    n = len(code)
    while i < n and code[i].isspace():
        i += 1
    return i


def skip_directive(code: str, i: int) -> int:
    """Skip a preprocessor line, including its backslash continuations."""
    n = len(code)
    while i < n:
        if code[i] == "\\" and i + 1 < n and code[i + 1] == "\n":
            i += 2
            continue
        if code[i] == "\n":
            return i + 1
        i += 1
    return n


def skip_balanced(code: str, i: int) -> int:
    """*i* is at an opening bracket; return the offset just past its mate."""
    pairs = {"(": ")", "[": "]", "{": "}"}
    stack: List[str] = []
    n = len(code)
    while i < n:
        c = code[i]
        if c in pairs:
            stack.append(pairs[c])
        elif c in ")]}":
            if stack and stack[-1] == c:
                stack.pop()
                if not stack:
                    return i + 1
        i += 1
    return n


def skip_to_semicolon(code: str, i: int) -> int:
    """Skip to just past the next statement-level `;` (for non-functions)."""
    n = len(code)
    depth = 0
    while i < n:
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth = max(0, depth - 1)
        elif c == ";" and depth == 0:
            return i + 1
        i += 1
    return n


@dataclass
class Decl:
    kind: str                  # definition | prototype | directive | block | other
    name: Optional[str]
    end: int                   # offset just past the declarator (or at its body)
    body: Optional[int] = None  # offset of the '{' of a definition


ATTRIBUTE_TAIL = re.compile(r"(?:__attribute__|__declspec)\s*$")
TRAILING_QUALIFIER = re.compile(r"(?:const|volatile|noexcept|override|final)\b")
TRAILING_THROW = re.compile(r"throw\s*\([^)]*\)")
TRAILING_ATTRIBUTE = re.compile(r"(?:__attribute__|__declspec)\s*")


def parse_declarator(code: str, pos: int) -> Decl:
    """Classify the construct that starts exactly at *pos* (already unspaced).

    The name comes from the declarator itself -- the identifier just before
    its parameter list -- so the caller can compute the NID from the function
    that really follows the comment.
    """
    n = len(code)
    if pos >= n:
        return Decl("other", None, pos)
    if code[pos] == "#":
        return Decl("directive", None, skip_directive(code, pos))
    if code[pos] == "{":
        return Decl("block", None, pos)
    if re.match(r'extern\s*"C"\s*\{', code[pos:]):
        return Decl("block", None, pos)

    i = pos
    paren = -1
    while i < n:
        c = code[i]
        if c == ";":
            return Decl("other", None, i + 1)
        if c in "{}":
            # A struct/union/enum body or a bare block: not a declarator. The
            # tag is kept so the finding can say what follows instead.
            tag = re.search(r"\b(?:struct|union|enum)\s+([A-Za-z_]\w*)",
                            code[pos:i])
            return Decl("block", tag.group(1) if tag else None, i)
        if c == "=":
            return Decl("other", None, skip_to_semicolon(code, i))
        if c == "(":
            head = code[pos:i]
            if ATTRIBUTE_TAIL.search(head):
                i = skip_ws(code, skip_balanced(code, i))
                continue
            paren = i
            break
        i += 1
    if paren < 0:
        return Decl("other", None, n)

    head = code[pos:paren]
    if "=" in head or re.search(r"\btypedef\b", head):
        return Decl("other", None, skip_to_semicolon(code, paren))

    names = list(IDENT.finditer(head))
    if not names:
        return Decl("other", None, skip_to_semicolon(code, paren))
    match = names[-1]
    name = match.group(0)
    if name in KEYWORDS:
        return Decl("other", None, skip_to_semicolon(code, paren))
    # Only pointer/whitespace noise may sit between the name and its `(`
    # (`u32* f(`); a `)`/`,` means this was something else, e.g. a function
    # pointer variable.
    if head[match.end():].strip(" \t\r\n*&"):
        return Decl("other", None, skip_to_semicolon(code, paren))

    after = skip_ws(code, skip_balanced(code, paren))
    while True:
        m = (TRAILING_QUALIFIER.match(code, after)
             or TRAILING_THROW.match(code, after))
        if m:
            after = skip_ws(code, m.end())
            continue
        if TRAILING_ATTRIBUTE.match(code, after):
            bracket = code.find("(", after)
            after = n if bracket < 0 else skip_ws(code, skip_balanced(code, bracket))
            continue
        break

    if after < n and code[after] == "{":
        return Decl("definition", name, after + 1, after)
    if after < n and code[after] == ";":
        return Decl("prototype", name, after + 1)
    return Decl("other", name, skip_to_semicolon(code, after))


def iter_declarations(code: str) -> Iterator[Decl]:
    """Every top-level function declarator, in file order.

    Used only for the informative hint ("this value belongs to that other
    function in this file"), so it walks one level into `extern "C" { }` and
    steps over definitions and struct bodies.
    """
    i, n = 0, len(code)
    while i < n:
        i = skip_ws(code, i)
        if i >= n:
            break
        c = code[i]
        if c == "#":
            i = skip_directive(code, i)
            continue
        if c in ";}":
            i += 1
            continue
        if c == "{":
            i = skip_balanced(code, i)
            continue
        enter = re.match(r'extern\s*"C"\s*\{', code[i:])
        if enter:
            i += enter.end()
            continue
        decl = parse_declarator(code, i)
        if decl.name and decl.kind in ("definition", "prototype"):
            yield decl
        if decl.kind == "definition" and decl.body is not None:
            i = skip_balanced(code, decl.body)
        elif decl.kind == "block" and decl.end < n and code[decl.end] == "{":
            i = skip_balanced(code, decl.end)
        else:
            i = max(decl.end, i + 1)


# ---------------------------------------------------------------------------
# Findings
# ---------------------------------------------------------------------------

@dataclass
class Finding:
    kind: str                        # mismatch | orphan | malformed
    path: str
    line: int
    reason: Optional[str] = None     # orphan: why nothing binds
    text: Optional[str] = None       # malformed: the comment as written
    function: Optional[str] = None
    comment_nid: Optional[int] = None
    computed_nid: Optional[int] = None
    hint_function: Optional[str] = None
    value_start: int = -1            # the 0xXXXXXXXX token, for --fix
    value_end: int = -1


@dataclass
class Annotation:
    """A NID annotation: its value (None when unreadable) and value offsets."""
    value: Optional[int]
    value_start: int
    value_end: int


def read_annotation(body: str, body_offset: int) -> Optional[Annotation]:
    """Read a NID annotation out of a comment body, or None if it is prose."""
    head = ANNOTATION_HEAD.match(body)
    if not head:
        return None
    value = NID_VALUE.match(body, head.end())
    if not value:
        return Annotation(None, -1, -1)
    return Annotation(int(value.group(1), 16),
                      body_offset + value.start(), body_offset + value.end())


def analyse_file(path: str, display: str) -> Tuple[str, int, List[Finding]]:
    """Return (file text, number of NID annotations, findings) for one file."""
    try:
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
    except OSError as exc:
        raise CheckError(f"cannot read {display}: {exc}") from exc

    comments, code = lex(text)
    line_starts = [0]
    for idx, ch in enumerate(text):
        if ch == "\n":
            line_starts.append(idx + 1)

    def line_of(offset: int) -> int:
        return bisect.bisect_right(line_starts, offset)

    def line_start(offset: int) -> int:
        return line_starts[line_of(offset) - 1]

    extension = os.path.splitext(path)[1].lower()
    own_nid = {}
    for decl in iter_declarations(code):
        own_nid.setdefault(decl.name, compute_nid(decl.name))

    findings: List[Finding] = []
    annotations = 0
    for comment in comments:
        body = text[comment.start + 2:comment.end - 2]
        annotation = read_annotation(body, comment.start + 2)
        if annotation is None:
            continue
        annotations += 1
        line = line_of(comment.start)

        if annotation.value is None:
            findings.append(Finding(
                "malformed", display, line, text=text[comment.start:comment.end],
                comment_nid=None))
            continue

        def orphan(reason: str, name: Optional[str] = None) -> None:
            findings.append(Finding("orphan", display, line, reason=reason,
                                    function=name))

        # A trailing comment annotates nothing that follows it.
        if text[line_start(comment.start):comment.start].strip():
            orphan("code precedes it on the line")
            continue

        pos = skip_ws(code, comment.end)
        gap = text[comment.end:pos]
        if "/*" in gap or "//" in gap:
            orphan("another comment comes between it and the function")
            continue
        if pos >= len(code):
            orphan("end of file")
            continue

        decl = parse_declarator(code, pos)
        if decl.kind == "directive":
            orphan("a preprocessor directive comes between it and the function",
                   decl.name)
            continue
        if decl.kind == "block":
            orphan("a struct, union, enum or block follows it, not a function",
                   decl.name)
            continue
        if decl.kind == "other" or not decl.name:
            orphan("no function declaration follows it")
            continue
        if decl.kind == "prototype" and extension in IMPLEMENTATION_EXTENSIONS:
            orphan("a prototype follows it, not the definition", decl.name)
            continue

        computed = compute_nid(decl.name)
        if computed == annotation.value:
            continue
        hint = next((name for name, nid in own_nid.items()
                     if nid == annotation.value and name != decl.name), None)
        findings.append(Finding(
            "mismatch", display, line, function=decl.name,
            comment_nid=annotation.value, computed_nid=computed,
            hint_function=hint, value_start=annotation.value_start,
            value_end=annotation.value_end))

    findings.sort(key=lambda f: (f.line, f.kind))
    return text, annotations, findings


# ---------------------------------------------------------------------------
# Paths, output, fixing
# ---------------------------------------------------------------------------

def collect_files(paths: List[str]) -> List[str]:
    files: List[str] = []
    for given in paths:
        if os.path.isdir(given):
            for root, dirs, names in os.walk(given):
                dirs.sort()
                for name in sorted(names):
                    candidate = os.path.join(root, name)
                    if os.path.splitext(name)[1].lower() in SCANNED_EXTENSIONS:
                        files.append(candidate)
        elif os.path.isfile(given):
            files.append(given)
        else:
            raise CheckError(f"no such file or directory: {given}")
    return files


def display_path(path: str) -> str:
    return path.replace(os.sep, "/")


def render_hex(nid: int, original: str) -> str:
    """The new value in the comment's own notation (prefix and letter case)."""
    digits = f"{nid:08X}"
    if any(c in "abcdef" for c in original[2:]):
        digits = digits.lower()
    return original[:2] + digits


def apply_fixes(text: str, findings: List[Finding]) -> Tuple[str, List[dict]]:
    """Rewrite mismatch values in place; return (new text, what changed).

    Only the value token is replaced, so the comment keeps its exact form --
    the colon or its absence, the spacing, the trailing prose. Descending
    offsets keep the earlier spans valid while the text shrinks or grows.
    """
    fixed = []
    for finding in sorted((f for f in findings
                           if f.kind == "mismatch" and f.value_start >= 0),
                          key=lambda f: f.value_start, reverse=True):
        original = text[finding.value_start:finding.value_end]
        replacement = render_hex(finding.computed_nid, original)
        if replacement == original:
            continue
        text = (text[:finding.value_start] + replacement
                + text[finding.value_end:])
        fixed.append({"path": finding.path, "line": finding.line,
                      "function": finding.function,
                      "from": original, "to": replacement})
    fixed.reverse()
    return text, fixed


def human_finding(finding: Finding) -> str:
    if finding.kind == "mismatch":
        line = (f"{finding.path}:{finding.line}: mismatch: {finding.function}: "
                f"comment {hex32(finding.comment_nid)}, computed "
                f"{hex32(finding.computed_nid)}")
        if finding.hint_function:
            line += (f" ({hex32(finding.comment_nid)} is "
                     f"{finding.hint_function}'s)")
        return line
    if finding.kind == "orphan":
        line = f"{finding.path}:{finding.line}: orphan: {finding.reason}"
        return line + (f": {finding.function}" if finding.function else "")
    return f"{finding.path}:{finding.line}: malformed: {finding.text}"


def finding_json(finding: Finding) -> dict:
    # Every key is always present, in this order: a consumer may rely on the
    # schema without probing for absent fields.
    return {
        "kind": finding.kind,
        "path": finding.path,
        "line": finding.line,
        "function": finding.function,
        "comment_nid": hex32(finding.comment_nid),
        "computed_nid": hex32(finding.computed_nid),
        "hint_function": finding.hint_function,
        "reason": finding.reason,
        "text": finding.text,
    }


def run(args) -> int:
    paths = args.paths or list(DEFAULT_PATHS)
    files = collect_files(paths)

    annotations = 0
    findings: List[Finding] = []
    fixes: List[dict] = []
    for path in files:
        display = display_path(path)
        text, count, found = analyse_file(path, display)
        annotations += count
        if args.fix:
            updated, changed = apply_fixes(text, found)
            if changed:
                try:
                    with open(path, "w", encoding="utf-8",
                              errors="surrogateescape") as f:
                        f.write(updated)
                except OSError as exc:
                    raise CheckError(f"cannot write {display}: {exc}") from exc
                fixes.extend(changed)
                fixed_lines = {item["line"] for item in changed}
                found = [f for f in found
                         if not (f.kind == "mismatch" and f.line in fixed_lines)]
        findings.extend(found)

    counts = {"mismatch": 0, "orphan": 0, "malformed": 0}
    for finding in findings:
        counts[finding.kind] += 1

    if args.json:
        payload = {
            "schema": SCHEMA,
            "files": len(files),
            "annotations": annotations,
            "fix": bool(args.fix),
            "findings": [finding_json(f) for f in findings],
            "fixed": fixes,
            "counts": counts,
        }
        json.dump(payload, sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        for item in fixes:
            print(f"fixed {item['path']}:{item['line']}: {item['function']}: "
                  f"{item['from']} -> {item['to']}")
        for finding in findings:
            print(human_finding(finding))
        summary = (f"checked {len(files)} file(s), {annotations} NID "
                   f"annotation(s): {counts['mismatch']} mismatch, "
                   f"{counts['orphan']} orphan, {counts['malformed']} malformed")
        if args.fix:
            summary += f", {len(fixes)} fixed"
        print(summary)
    return 1 if findings else 0


def main(argv=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="""\
Check that every /* NID: 0x... */ comment states the NID computed from the
name of the function it documents (tools/nid_database.compute_nid, which is
also what registration uses).

A comment binds to the function that immediately follows it: only blank lines
may come between them. A trailing comment, another comment, a preprocessor
directive, a struct, a macro or any other construct makes it an orphan, since
then the annotated function is not the one that follows. In an implementation
file (.c/.cpp/.m/.mm) the comment must sit on a definition -- a prototype
there is an orphan, because computing a NID from a forward declaration would
invent a plausible-looking value. A header has no body to attach to, so its
declarations are checked in place. Only a comment whose body BEGINS with NID
is an annotation; a NID quoted in prose is prose.

Findings: mismatch (the comment disagrees with the computed NID, with a hint
naming the function in the same file whose NID it actually is), orphan
(nothing binds), malformed (no 0x and eight hex digits after NID).""",
        epilog="""\
--fix rewrites only mismatch values, in place, and preserves the comment's
exact form (prefix, letter case, spacing, trailing prose). It is idempotent,
and it never touches orphans or malformed comments: there is no right value to
write for those.

Exit status: 0 clean, 1 findings, 2 usage or I/O error.""")
    parser.add_argument(
        "paths", nargs="*", metavar="PATH",
        help="files or directories to check (default: libs/)")
    parser.add_argument(
        "--fix", action="store_true",
        help="rewrite mismatch values to the computed NID, in place; orphans "
             "and malformed comments are never rewritten")
    parser.add_argument(
        "--json", action="store_true",
        help=f"print a JSON document (schema {SCHEMA})")
    args = parser.parse_args(argv)
    try:
        return run(args)
    except CheckError as exc:
        print(f"check_nid_comments: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
