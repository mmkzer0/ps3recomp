#!/usr/bin/env python3
"""hle_check_audit.py -- evidence audit of the checks HLE handlers make.

Every early error return or fail-closed stop in an HLE handler is a claim about
the firmware.  A check the firmware does not make is a divergence: the poolAttr
alignment check (f1a51f1) failed Odin's second queue because libspurs_jq's
CreateCommon never aligns the pool attribute, and the JQ2 create order/codes
(fe3a78f) returned invented codes for pool/pool_attr pairings.  This tool finds
the checks and classifies the comment that justifies each one, so the ones with
no evidence can be reviewed against firmware before they are changed.

Scope
-----
A handler is a function the generated NID table registers.  The set is
reconstructed exactly like tools/gen_hle_nids.py: every libs/<dir>/cell*|sce*|
sys*.c module, every non-static definition whose name classify_name() calls a
firmware export (host-side helpers are excluded).  Handlers registered by
runtime/ppu/ppu_sysprx.cpp's ps3_hle_register_ctx (the sysPrxForUser, CRT and
network context surface) are NOT in that table and are not scanned; the JSON
reports their NIDs as unmatched when --imports names them.

Check shapes found (syntactic, one level of `if` consequence):
  * `if (cond) return <error>;`
  * `if (cond) { ... return <error>; ... }`
  * `if (cond) ps3_stop*();`
  * the same, brace-less or with the action on its own line
"error" is a numeric 0x8....... return, a CELL_/SCE_/JQ2_/... symbol whose name
carries ERR/ERROR/FAULT, or CELL_E*; CELL_OK/_OK/success values are not checks.
The scope field separately marks whether the condition names a declared
parameter.  It is a hint only: a condition on a local derived from a parameter
reads as "state" (see Limits).

Evidence classes, from the nearest comment that carries evidence, in order of
specificity: the action's own comment, the `if`'s comment (both with only
whitespace between comment and code), a trailing same-line comment, the
handler's leading comment, then the file header.  The file header is reported
with source="file" because it is the module's contract, not a claim about one
check.
  fw-ea          a firmware address 0x0200xxxx (or 0200xxxx), or a firmware
                 version/module plus a KB or work|docs document reference.
  oracle-rpcs3   names RPCS3 and the file/function consulted.
  sdk            names the SDK and a header/prototype/documented convention.
  none           no evidence.

Output: JSON (machine-checkable, full evidence) and a Markdown table.

Usage:
  python3 tools/hle_check_audit.py --json out.json --md out.md
  python3 tools/hle_check_audit.py --imports reached-nids.txt \
          --json reached.json --md reached.md
  python3 tools/hle_check_audit.py --classes none --md no-evidence.md

Limits (also recorded in the JSON under "limits"):
  - A comment is attached only when it is contiguous with the action (only
    whitespace between).  A file header or function comment separated by the
    signature is not evidence for a check inside the body.
  - Only `if` consequences are scanned.  `else`, ternary, switch-case, goto
    error paths, and loops that end in an error return are not checks here.
  - Calls into host helpers are not followed.  jq2_queue_create's checks (the
    other half of the JQ2 create divergence) live in libs/spurs/jq2/executor.cpp,
    which defines no registered handler, so they are outside this scan.
  - A string/comment-aware brace matcher handles literals, but `#if 0` blocks
    and macro bodies are not expanded or disabled.
  - `uses_param` is a word match against the declared parameter names; a
    condition on a local computed from a parameter is marked "state".
  - Non-ASCII source is read as latin-1 (the same convention as
    gen_hle_nids.py).

Exit status is 0 when the audit ran (the `none` class is data, not a tool
failure), 2 on a malformed --imports file.
"""

from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import sys
from collections import Counter, OrderedDict
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import gen_hle_nids as gen  # noqa: E402

SCHEMA = "ps3recomp.hle-check-audit/1"

CLASSES = ("fw-ea", "oracle-rpcs3", "sdk", "none")

LIMITS = [
    "A comment is attached only when only whitespace separates it from the "
    "action (comment above the if or above the action) or when it trails the "
    "action on the same line. The handler's leading comment (contiguous before "
    "the signature) and the file header are weaker fallbacks; the JSON records "
    "which one via evidence_scope. A section comment above an earlier function "
    "is not propagated, so a group-level claim (e.g. cellSpursJq.c's 'each checks "
    "only attr != NULL') shows up as none for the later functions -- a false "
    "positive for review, never a false pass.",
    "A comment separated from the action by a declaration or another statement "
    "(a multi-statement validation block) is not attached: only the action, "
    "the if, the trailing line, the function's leading comment and the file "
    "header are tried.",
    "Evidence is syntactic: the first attached scope (action, if, trailing, "
    "function, file) whose comment contains a firmware EA/RPCS3/SDK reference "
    "passes, even if the sentence is about a different check. If none carries "
    "evidence, the nearest comment is reported with class none. The evidence "
    "text and source let the follow-up judge the match.",
    "Only the consequent of an `if` is scanned. `else`, ternary, switch-case, "
    "goto error paths and loop-condition exits are not classified.",
    "Calls into host helpers are not followed. jq2_queue_create (the implementation "
    "of _cellSpursCreateJobQueue*) lives in libs/spurs/jq2/executor.cpp, which "
    "defines no registered handler, so its checks are outside the scan.",
    "Handlers registered by ps3_hle_register_ctx in runtime/ppu/ppu_sysprx.cpp "
    "(sysPrxForUser/CRT/network context surface) are not in the generated libs "
    "NID table and are not scanned; their --imports NIDs are reported unmatched.",
    "`#if 0` blocks and macro bodies are not preprocessed; a literal `if` there "
    "would be scanned. The tree has none under libs/ today, but the claim is "
    "syntactic.",
    "uses_param is a word match against the declared parameter names. A condition "
    "on a local derived from a parameter (ea = (u32)ptr; if (ea ...)) reads as "
    "state; the check is still listed.",
    "Source is read as latin-1, matching gen_hle_nids.py.",
]


# ---------------------------------------------------------------------------
# Source masking and comment extraction
# ---------------------------------------------------------------------------

def mask_source(text: str) -> str:
    """Return a same-length copy with comments and literals blanked to spaces.

    Blanking (not deleting) keeps every index and line number usable for the
    original text.  Newlines survive so line mapping is unchanged.
    """
    out = list(text)
    n = len(text)

    def blank(a: int, b: int) -> None:
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    i = 0
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i + 2)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"' or text[j] == "\n":
                    j += 1
                    break
                j += 1
            blank(i, j)
            i = j
        elif c == "'":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'" or text[j] == "\n":
                    j += 1
                    break
                j += 1
            blank(i, j)
            i = j
        else:
            i += 1
    return "".join(out)


@dataclass
class Comment:
    start: int
    end: int
    text: str
    line_start: int
    line_end: int


def collect_comments(text: str, lines: "LineIndex") -> List[Comment]:
    comments: List[Comment] = []
    n = len(text)
    i = 0
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i + 2)
            j = n if j < 0 else j
            body = text[i + 2:j]
            comments.append(Comment(i, j, body, lines.line(i), lines.line(max(i, j - 1))))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            body = text[i + 2:max(i + 2, j - 2)]
            comments.append(Comment(i, j, body, lines.line(i), lines.line(max(i, j - 1))))
            i = j
        elif c == '"' or c == "'":
            quote = c
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote or text[j] == "\n":
                    j += 1
                    break
                j += 1
            i = j
        else:
            i += 1
    return comments


class LineIndex:
    def __init__(self, text: str) -> None:
        self.starts = [0]
        for i, c in enumerate(text):
            if c == "\n":
                self.starts.append(i + 1)

    def line(self, pos: int) -> int:
        return bisect.bisect_right(self.starts, pos)


# ---------------------------------------------------------------------------
# Brace / statement matching
# ---------------------------------------------------------------------------

def find_matching(mask: str, pos: int, open_ch: str, close_ch: str) -> int:
    """Index of the close_ch matching mask[pos] == open_ch, or -1."""
    depth = 0
    for i in range(pos, len(mask)):
        if mask[i] == open_ch:
            depth += 1
        elif mask[i] == close_ch:
            depth -= 1
            if depth == 0:
                return i
    return -1


def _last_nonspace(mask: str, before: int, floor: int) -> str:
    i = before - 1
    while i >= floor and mask[i].isspace():
        i -= 1
    return mask[i] if i >= floor else ""


def statement_end(mask: str, start: int, limit: int) -> int:
    """End index (exclusive) of one top-level statement/construct at start.

    A top-level `{` preceded by anything but '=', ',', '(' or '{' is a control
    construct's body and ends the statement at its matching '}'.  A `{` after
    '='/','/'(' is an initializer and the statement continues to its `;`.
    """
    paren = bracket = 0
    i = start
    while i < limit:
        c = mask[i]
        if c == "(":
            paren += 1
        elif c == ")":
            paren -= 1
        elif c == "[":
            bracket += 1
        elif c == "]":
            bracket -= 1
        elif c == "{" and paren == 0 and bracket == 0:
            close = find_matching(mask, i, "{", "}")
            if close < 0 or close >= limit:
                return limit
            prev = _last_nonspace(mask, i, start)
            if prev not in "=,({":
                return close + 1
            i = close
        elif c == ";" and paren == 0 and bracket == 0:
            return i + 1
        i += 1
    return limit


def top_level_statements(mask: str, start: int, end: int) -> Iterable[Tuple[int, int]]:
    i = start
    while i < end:
        while i < end and mask[i].isspace():
            i += 1
        if i >= end:
            return
        if mask[i] == "}":
            return
        j = statement_end(mask, i, end)
        if j <= i:
            return
        yield i, j
        i = j


def in_preprocessor_line(mask: str, pos: int) -> bool:
    start = mask.rfind("\n", 0, pos) + 1
    return mask[start:pos].lstrip().startswith("#")


# ---------------------------------------------------------------------------
# Handler discovery
# ---------------------------------------------------------------------------

@dataclass
class Handler:
    name: str
    nid: int
    rel: str           # repository-relative
    text: str
    mask: str
    def_start: int
    body_start: int
    body_end: int      # index of the closing '}'
    params: List[str]


_FUNC_RE = gen.DEF_RE


def parse_param_names(signature: str) -> List[str]:
    """Declared parameter names from a C definition signature.

    The text passed is everything between the handler's outer parentheses.
    Function-pointer parameters (`void (*cb)(int)`) still yield `cb`; unnamed
    parameters yield nothing.  Only a word use in the condition is scored, so a
    missing name can only turn a parameter check into `state`.
    """
    sig = ' '.join(signature.split())
    if not sig or sig.strip() == "void":
        return []
    # Split top-level commas.
    parts: List[str] = []
    depth = 0
    cur = []
    for ch in sig:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append(''.join(cur))
    names: List[str] = []
    for part in parts:
        p = part.strip()
        if not p or p == "void":
            continue
        # Function pointer: `ret (*name)(args)`.
        m = re.search(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)", p)
        if m:
            names.append(m.group(1))
            continue
        idents = re.findall(r"[A-Za-z_]\w*", p)
        if not idents:
            continue
        # Drop a lone type (`int`, `const char *`) -- no name to score.
        if len(idents) == 1 and re.fullmatch(
                r"(?:const|volatile|unsigned|signed|void|char|short|int|long|"
                r"float|double|bool|_Bool|u?int\d*_t|s\d+|u\d+|size_t|ssize_t|"
                r"uintptr_t|intptr_t)", idents[0]):
            continue
        names.append(idents[-1])
    return names


def discover_handlers(root: str) -> Tuple[List[Handler], List[str]]:
    """Registered libs handlers with their definition body, plus warnings."""
    warnings: List[str] = []
    known = gen.known_export_names()
    handlers: List[Handler] = []
    seen_nids: Dict[int, str] = {}
    for lib in gen.discover_all(root):
        path = gen.module_source(lib, root)
        if not path:
            continue
        try:
            text = open(path, encoding="latin1").read()
        except OSError as e:
            warnings.append(f"cannot read {path}: {e}")
            continue
        mask = mask_source(text)
        lines = LineIndex(text)
        matches: Dict[str, re.Match] = {}
        for m in _FUNC_RE.finditer(mask):
            matches.setdefault(m.group(1), m)
        for nid, name in gen.scan(path):
            if nid in seen_nids:
                if seen_nids[nid] != name:
                    warnings.append(
                        f"NID collision 0x{nid:08X}: {seen_nids[nid]} and {name}; "
                        f"the generated table keeps the first")
                continue
            try:
                kind = gen.classify_name(name, known)
            except gen.UnclassifiedName as e:
                warnings.append(f"skipping unclassified definition: {e}")
                continue
            if kind != "export":
                continue
            m = matches.get(name)
            if not m:
                warnings.append(f"{name}: scanned but no definition match")
                continue
            body_open = m.end() - 1
            if text[body_open] != "{":
                body_open = text.find("{", m.end() - 1)
            if body_open < 0:
                warnings.append(f"{name}: no body")
                continue
            body_end = find_matching(mask, body_open, "{", "}")
            if body_end < 0:
                warnings.append(f"{name}: unbalanced body")
                continue
            # Parameter names: outer parentheses of the definition head.
            head = text[m.start():body_open]
            close = head.rfind(")")
            open_paren = head.rfind("(", 0, close)
            params = parse_param_names(head[open_paren + 1:close]) if open_paren >= 0 else []
            rel = os.path.relpath(path, root)
            handlers.append(Handler(name, nid, rel, text, mask, m.start(),
                                    body_open, body_end, params))
            seen_nids[nid] = name
    handlers.sort(key=lambda h: (h.rel, h.name))
    return handlers, warnings


# ---------------------------------------------------------------------------
# Error-code and stop recognition
# ---------------------------------------------------------------------------

_CAST_RE = re.compile(
    r"^\(\s*(?:const\s+|volatile\s+|unsigned\s+|signed\s+)*"
    r"(?:void|char|short|int|long|float|double|bool|_Bool|"
    r"u?int(?:8|16|32|64)_t|s(?:8|16|32|64)|u(?:8|16|32|64)|"
    r"size_t|ssize_t|u?intptr_t|CellError|error_code)\s*\*?\s*\)\s*(.+)$")


def strip_casts(expr: str) -> str:
    s = ' '.join(expr.split())
    while True:
        m = _CAST_RE.match(s)
        if not m:
            return s
        s = m.group(1).strip()


_ERR_TOKEN_RE = re.compile(r"(?:^|_)(?:ERR|ERROR|FAULT)(?:_|$)")
_E_FAMILY_RE = re.compile(r"^(?:CELL|SCE)_E[A-Z0-9_]*$")


def error_code(expr: str) -> Optional[str]:
    """The firmware-visible error expression, or None if this is not one.

    Recognised: a Cell-style 0x8....... code (with or without a u/U suffix), or
    a symbol whose name carries ERR/ERROR/FAULT as a component, or the
    CELL_E/SCE_E family.  A variable (`return rc`) is deliberately not a code:
    it has no literal to compare with firmware, and reporting it as a "check"
    would invent one.  0xFFFFFFFF is not accepted: it is a sentinel, not a
    Cell error, and would make every `return -1`-shaped path a check.
    """
    s = strip_casts(expr)
    if not s:
        return None
    if re.fullmatch(r"0[xX]8[0-9A-Fa-f]{7}[uUlL]*", s):
        return s
    if re.fullmatch(r"-?\d+[uUlL]*", s):
        return None
    if not re.fullmatch(r"[A-Za-z_]\w*", s):
        return None
    name = s
    if name in ("CELL_OK", "NULL", "nullptr"):
        return None
    if re.search(r"(?:^|_)OK(?:_|$)", name):
        return None
    if name.startswith(("CELL_", "SCE_")):
        if name in ("CELL_OK",):
            return None
        if _ERR_TOKEN_RE.search(name) or _E_FAMILY_RE.match(name):
            return name
        # CELL_* symbols other than OK are status codes in this tree; only
        # accept the ones that name a rejection, not e.g. CELL_GCM_FLAG_*.
        if re.search(r"(?:^|_)(?:INVALID|PARAM|NOT_INITIALIZED|UNINITIALIZED|"
                     r"BUSY|FAILURE|ALIGN|STAT|SEQ|UNSET|NOT_FOUND|NOT_OPENED|"
                     r"OUT_OF_RANGE|TOO_|FULL|EMPTY|EXISTS|TIMEOUT|ABORT|"
                     r"CANCELED|DENIED|PERM|DEADLK|AGAIN|NOMEM|NOSYS|NOENT|"
                     r"IO|SRCH|DOM|RANGE|FAULT|ALIGNMENT|NOT_SUPPORTED|"
                     r"UNSUPPORTED|BAD_ARGUMENT|INVALID_ARGUMENT)(?:_|$)", name):
            return name
        return None
    if _ERR_TOKEN_RE.search(name):
        return name
    return None


def stop_call(mask: str, text: str, start: int, end: int) -> Optional[Tuple[str, int]]:
    """(stop code, statement end) when the statement is a ps3_stop* call."""
    s = start
    while s < end and mask[s].isspace():
        s += 1
    m = re.match(r"(ps3_stop\w*)\s*\(", mask[s:end])
    if not m:
        return None
    name = m.group(1)
    open_paren = s + m.end() - 1
    close = find_matching(mask, open_paren, "(", ")")
    if close < 0 or close > end:
        return None
    if name == "ps3_stop":
        args = text[open_paren + 1:close]
        first = args.split(",", 1)[0].strip()
        first = ' '.join(first.split())
        code = first if first else name
    else:
        code = name
    stmt_end = close + 1
    while stmt_end < end and mask[stmt_end].isspace():
        stmt_end += 1
    if stmt_end < end and mask[stmt_end] == ";":
        stmt_end += 1
    return code, stmt_end


# ---------------------------------------------------------------------------
# Evidence classification
# ---------------------------------------------------------------------------

# Firmware images are linked at 0x02000000 in fw/lift; evidence comments use
# either `0x020036D4` or the bare `020045E8` form (cellResc.c:628).
FW_EA_RE = re.compile(r"\b(?:0[xX])?020[0-9A-Fa-f]{5}\b")
FW_VERSION_RE = re.compile(r"\bFW\s*\d+\.\d+|\bfirmware\b", re.I)
FW_DOC_RE = re.compile(
    r"\bKB\s+[a-z][a-z0-9_.-]*|\b(?:work|docs)/[A-Za-z0-9_./-]+"
    r"\.(?:md|txt|json|dis)\b", re.I)
FW_MODULE_RE = re.compile(
    r"\blib(?:gcm_sys|resc|spurs_jq|fiber|sync2|sre)\b", re.I)
RPCS3_RE = re.compile(r"\brpcs3\b", re.I)
RPCS3_REF_RE = re.compile(
    r"\b[\w.-]+\.(?:cpp|cc|cxx|c|h|hpp)\b|"
    r"\brpcs3/|\b[A-Za-z_]\w*\(|\b[A-Za-z_]\w*::[A-Za-z_]\w*")
SDK_RE = re.compile(r"\bSDK\b")
SDK_REF_RE = re.compile(
    r"\bcell/[\w/.-]+\.h\b|\b[\w.-]+\.h\b|\bprototype\b|\bdocumented\b|"
    r"\bofficial\b|\bSDK samples?\b|\bgcm_implementation\w*\b", re.I)


def classify_evidence(text: str) -> Tuple[str, str]:
    """(class, basis) for one comment group.  Strongest evidence wins."""
    if not text:
        return "none", ""
    if FW_EA_RE.search(text):
        return "fw-ea", "ea"
    if FW_VERSION_RE.search(text) and (FW_DOC_RE.search(text) or FW_MODULE_RE.search(text)):
        return "fw-ea", "doc-ref"
    if RPCS3_RE.search(text) and RPCS3_REF_RE.search(text):
        return "oracle-rpcs3", "file/function"
    if RPCS3_RE.search(text):
        return "none", "rpcs3-without-file/function"
    if SDK_RE.search(text) and SDK_REF_RE.search(text):
        return "sdk", "sdk-reference"
    return "none", ""


# ---------------------------------------------------------------------------
# Check extraction
# ---------------------------------------------------------------------------

@dataclass
class Check:
    handler: str
    nid: int
    file: str
    line: int
    if_line: int
    condition: str
    code: str
    kind: str              # return | stop
    scope: str             # param | state
    cls: str
    basis: str
    evidence: str
    evidence_line: Optional[int]
    evidence_scope: str
    module: str

    def as_dict(self) -> "OrderedDict":
        d = OrderedDict()
        d["handler"] = self.handler
        d["nid"] = f"0x{self.nid:08X}"
        d["file"] = self.file
        d["line"] = self.line
        d["if_line"] = self.if_line
        d["condition"] = self.condition
        d["code"] = self.code
        d["kind"] = self.kind
        d["scope"] = self.scope
        d["class"] = self.cls
        d["basis"] = self.basis
        d["evidence_line"] = self.evidence_line
        d["evidence_scope"] = self.evidence_scope
        d["evidence"] = self.evidence
        return d


def _comments_before(pos: int, comments: Sequence[Comment], mask: str) -> List[Comment]:
    """The contiguous comment group ending immediately before pos.

    Walks backwards through comments whose only in-between text is whitespace.
    A code token between a comment and pos stops the walk: an earlier comment
    belongs to the earlier statement, not this one.
    """
    group: List[Comment] = []
    limit = pos
    for c in reversed(comments):
        if c.end > limit:
            continue
        if mask[c.end:limit].strip():
            break
        group.append(c)
        limit = c.start
    group.reverse()
    return group


def _trailing_comment(pos: int, comments: Sequence[Comment],
                      mask: str, lines: "LineIndex") -> Optional[Comment]:
    line = lines.line(pos)
    for c in comments:
        if c.start < pos:
            continue
        if lines.line(c.start) != line:
            break
        if not mask[pos:c.start].strip():
            return c
        break
    return None


def _top_level_groups(text: str, mask: str, comments: Sequence[Comment],
                      lines: "LineIndex") -> List[List[Comment]]:
    """Comment groups that start at column 0 (file/section headers).

    Only the first group (the file header) is used as evidence; a later
    column-0 section comment belongs to the declaration that follows it and is
    already found by the contiguous leading-comment rule.
    """
    groups: List[List[Comment]] = []
    cur: List[Comment] = []
    for c in comments:
        line_start = lines.starts[c.line_start - 1]
        if c.start != line_start:
            continue        # indented: a comment inside a function, not a section
        if cur and not mask[cur[-1].end:c.start].strip():
            cur.append(c)
        else:
            if cur:
                groups.append(cur)
            cur = [c]
    if cur:
        groups.append(cur)
    return groups


@dataclass
class EvidenceContext:
    leading: List[Comment]
    file_header: List[Comment]


def _make_context(handler: Handler, comments: Sequence[Comment],
                  lines: "LineIndex") -> EvidenceContext:
    leading = _comments_before(handler.def_start, comments, handler.mask)
    file_header: List[Comment] = []
    for group in _top_level_groups(handler.text, handler.mask, comments, lines):
        if group[0].line_start == 1:
            file_header = group
        break
    return EvidenceContext(leading, file_header)


def _group_text(group: Sequence[Comment]) -> str:
    text = ' '.join(' '.join(c.text.split()) for c in group).strip()
    # A banner (`/* ---- File operations ---- */`) is layout, not a sentence.
    return ' '.join(re.sub(r"[-=*_~#]{3,}", " ", text).split())


def _pick_evidence(candidates: Sequence[Tuple[str, Sequence[Comment]]]
                   ) -> Tuple[str, str, Optional[int], str, str]:
    """First candidate group whose comment carries evidence.

    Candidates are ordered by specificity (action, if, trailing, function
    comment, file header).  The function comment must still precede the
    signature with only whitespace between; the file header is the module's own
    contract statement and is reported with source="file" so a reviewer can
    tell it is not check-specific.  If none carries evidence, the nearest
    informative group's text is still reported.
    """
    nearest_text = ""
    nearest_line: Optional[int] = None
    nearest_scope = ""
    for scope, group in candidates:
        if not group:
            continue
        text = _group_text(group)
        if not text or not re.search(r"[A-Za-z0-9]", text):
            continue
        if nearest_line is None:
            nearest_text, nearest_line, nearest_scope = text, group[0].line_start, scope
        cls, basis = classify_evidence(text)
        if cls != "none":
            return text, cls, group[0].line_start, basis, scope
    return nearest_text, "none", nearest_line, "", nearest_scope


def _scope(condition: str, params: Sequence[str]) -> str:
    words = set(re.findall(r"[A-Za-z_]\w*", condition))
    return "param" if any(p in words for p in params) else "state"


def extract_checks(handler: Handler, comments: Sequence[Comment],
                   lines: "LineIndex") -> List[Check]:
    mask = handler.mask
    text = handler.text
    body_start = handler.body_start
    body_end = handler.body_end
    ctx = _make_context(handler, comments, lines)
    checks: List[Check] = []
    for m in re.finditer(r"(?<![\w#])if\s*\(", mask[body_start + 1:body_end]):
        if_pos = body_start + 1 + m.start()
        if in_preprocessor_line(mask, if_pos):
            continue
        open_paren = body_start + 1 + m.end() - 1
        close_paren = find_matching(mask, open_paren, "(", ")")
        if close_paren < 0 or close_paren >= body_end:
            continue
        condition = ' '.join(text[open_paren + 1:close_paren].split())
        q = close_paren + 1
        while q < body_end and mask[q].isspace():
            q += 1
        if q >= body_end:
            continue
        if mask[q] == "{":
            block_close = find_matching(mask, q, "{", "}")
            if block_close < 0:
                continue
            for s, e in top_level_statements(mask, q + 1, block_close):
                # Only the first direct action in the consequent belongs to
                # this condition; a later one is unreachable after a stop.
                tok = re.match(r"[A-Za-z_]\w*", mask[s:e].lstrip())
                if not tok:
                    continue
                if tok.group(0) == "return":
                    semi = mask.find(";", s, e)
                    if semi < 0:
                        continue
                    expr = text[mask.find("return", s, e) + len("return"):semi]
                    code = error_code(expr)
                    if code:
                        checks.append(self_check(
                            handler, condition, code, "return", if_pos,
                            s, semi + 1, ctx, comments, mask, lines))
                    break
                if tok.group(0).startswith("ps3_stop"):
                    stop = stop_call(mask, text, s, e)
                    if stop:
                        code, stmt_end = stop
                        checks.append(self_check(
                            handler, condition, code, "stop", if_pos,
                            s, stmt_end, ctx, comments, mask, lines))
                    break
                # A declaration or a nested construct: keep looking for the
                # first direct return/stop in this consequent. A nested `if` is
                # picked up by this same scan and is not an action of the outer
                # condition, so it is skipped here.
                continue
        else:
            stmt_end = statement_end(mask, q, body_end)
            tok = re.match(r"[A-Za-z_]\w*", mask[q:stmt_end].lstrip())
            if tok and tok.group(0) == "return":
                semi = mask.find(";", q, stmt_end)
                if semi >= 0:
                    expr = text[mask.find("return", q, semi) + len("return"):semi]
                    code = error_code(expr)
                    if code:
                        checks.append(self_check(
                            handler, condition, code, "return", if_pos,
                            q, semi + 1, ctx, comments, mask, lines))
            elif tok and tok.group(0).startswith("ps3_stop"):
                stop = stop_call(mask, text, q, stmt_end)
                if stop:
                    code, end = stop
                    checks.append(self_check(
                        handler, condition, code, "stop", if_pos,
                        q, end, ctx, comments, mask, lines))
    return checks


def self_check(handler: Handler, condition: str, code: str, kind: str,
               if_pos: int, action_start: int, action_end: int,
               ctx: EvidenceContext, comments: Sequence[Comment], mask: str,
               lines: "LineIndex") -> Check:
    trail = _trailing_comment(action_end, comments, mask, lines)
    evidence, cls, ev_line, basis, ev_scope = _pick_evidence([
        ("action", _comments_before(action_start, comments, mask)),
        ("if", _comments_before(if_pos, comments, mask)),
        ("trailing", [trail] if trail is not None else []),
        ("function", ctx.leading),
        ("file", ctx.file_header),
    ])
    return Check(
        handler=handler.name,
        nid=handler.nid,
        file=handler.rel,
        line=lines.line(action_start),
        if_line=lines.line(if_pos),
        condition=condition,
        code=code,
        kind=kind,
        scope=_scope(condition, handler.params),
        cls=cls,
        basis=basis,
        evidence=evidence,
        evidence_line=ev_line,
        evidence_scope=ev_scope,
        module=os.path.splitext(os.path.basename(handler.rel))[0],
    )


# ---------------------------------------------------------------------------
# Imports filter
# ---------------------------------------------------------------------------

class ImportsError(Exception):
    pass


def load_imports(path: str) -> List[int]:
    nids: List[int] = []
    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            try:
                value = int(line, 0) if line.lower().startswith("0x") else int(line, 16)
            except ValueError:
                raise ImportsError(
                    f"{path}:{lineno}: not a NID: {line!r} "
                    f"(expected one 0x-prefixed 32-bit value per line)")
            if not 0 <= value <= 0xFFFFFFFF:
                raise ImportsError(f"{path}:{lineno}: NID out of 32-bit range: {line!r}")
            nids.append(value)
    return nids


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def _md_cell(s: str, cap: int = 0) -> str:
    s = ' '.join(str(s).split())
    if cap and len(s) > cap:
        s = s[:cap - 1] + "…"
    return s.replace("|", "\\|").replace("`", "'")


def counts(checks: Sequence[Check]) -> "OrderedDict":
    by_class = Counter(c.cls for c in checks)
    by_kind = Counter(c.kind for c in checks)
    by_scope = Counter(c.scope for c in checks)
    by_file = Counter(c.file for c in checks)
    by_module = Counter(c.module for c in checks)
    d = OrderedDict()
    d["total"] = len(checks)
    d["by_class"] = OrderedDict((k, by_class.get(k, 0)) for k in CLASSES)
    d["by_kind"] = OrderedDict(sorted(by_kind.items()))
    d["by_scope"] = OrderedDict(sorted(by_scope.items()))
    d["by_file"] = OrderedDict(sorted(by_file.items()))
    d["by_module"] = OrderedDict(sorted(by_module.items()))
    return d


def build_report(root: str, handlers: Sequence[Handler], checks: Sequence[Check],
                 imports_path: Optional[str], imports: Optional[Sequence[int]],
                 unmatched: Sequence[int], warnings: Sequence[str]) -> "OrderedDict":
    rep = OrderedDict()
    rep["schema"] = SCHEMA
    rep["root"] = os.path.abspath(root)
    rep["imports_file"] = imports_path
    rep["imports_count"] = len(imports) if imports is not None else None
    rep["handlers_scanned"] = len(handlers)
    rep["handlers_with_checks"] = len({c.handler for c in checks})
    rep["checks"] = [c.as_dict() for c in checks]
    rep["counts"] = counts(checks)
    rep["unmatched_imports"] = [f"0x{n:08X}" for n in sorted(set(unmatched))]
    rep["warnings"] = list(warnings)
    rep["limits"] = list(LIMITS)
    return rep


def markdown(report: "OrderedDict", selected_classes: Sequence[str]) -> str:
    checks = report["checks"]
    c = report["counts"]
    lines: List[str] = []
    lines.append("# HLE parameter-check evidence audit")
    lines.append("")
    lines.append(f"Generated by `tools/hle_check_audit.py` ({SCHEMA}).")
    lines.append("")
    lines.append(f"- Root: `{report['root']}`")
    if report.get("imports_file"):
        lines.append(f"- Import filter: `{report['imports_file']}` "
                     f"({report['imports_count']} NIDs, "
                     f"{len(report['unmatched_imports'])} unmatched)")
    else:
        lines.append("- Import filter: none (whole tree)")
    lines.append(f"- Handlers scanned: {report['handlers_scanned']}; "
                 f"handlers with checks: {report['handlers_with_checks']}; "
                 f"checks: {c['total']}")
    lines.append("")
    lines.append("## Counts by class")
    lines.append("")
    lines.append("| class | checks |")
    lines.append("|---|---:|")
    for k in CLASSES:
        lines.append(f"| `{k}` | {c['by_class'].get(k, 0)} |")
    lines.append("")
    lines.append("By kind: " + ", ".join(f"`{k}` {v}" for k, v in c["by_kind"].items()))
    lines.append("")
    lines.append("By scope: " + ", ".join(f"`{k}` {v}" for k, v in c["by_scope"].items()))
    lines.append("")
    if selected_classes and set(selected_classes) != set(CLASSES):
        lines.append("Class filter: " + ", ".join(f"`{k}`" for k in selected_classes))
        lines.append("")
    if report["unmatched_imports"]:
        shown = report["unmatched_imports"]
        cap = 40
        lines.append("Unmatched import NIDs (not a registered libs handler): "
                     + ", ".join(f"`{n}`" for n in shown[:cap])
                     + (f" … {len(shown) - cap} more" if len(shown) > cap else ""))
        lines.append("")
    if report["warnings"]:
        lines.append("Warnings:")
        for w in report["warnings"]:
            lines.append(f"- {w}")
        lines.append("")
    lines.append("| handler | file:line | condition | code | class | source | evidence |")
    lines.append("|---|---|---|---|---|---|---|")
    for ch in checks:
        ev = ch["evidence"]
        ev_disp = ev if ev else "—"
        src = ch.get("evidence_scope") or "—"
        lines.append(
            f"| `{_md_cell(ch['handler'])}` | `{_md_cell(ch['file'])}:{ch['line']}` | "
            f"`{_md_cell(ch['condition'], 220)}` | `{_md_cell(ch['code'])}` | "
            f"`{ch['class']}` | `{src}` | {_md_cell(ev_disp, 220)} |")
    lines.append("")
    lines.append("## Limits")
    lines.append("")
    for lim in report["limits"]:
        lines.append(f"- {lim}")
    lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Audit the evidence behind every parameter check in the HLE "
                    "handlers (registered libs/** NID-table functions).")
    ap.add_argument("--root", default=ROOT,
                    help="tree whose libs/ is scanned (default: this checkout)")
    ap.add_argument("--imports", default=None,
                    help="plain-text NID list (one 0x... per line); restrict to "
                         "handlers whose computed NID is listed")
    ap.add_argument("--json", default=None, help="write the JSON report here")
    ap.add_argument("--md", default=None, help="write the Markdown table here")
    ap.add_argument("--classes", default=None,
                    help="comma-separated classes to include in the reports "
                         "(default: all; e.g. none)")
    args = ap.parse_args(argv)

    selected = CLASSES
    if args.classes:
        selected = tuple(x.strip() for x in args.classes.split(",") if x.strip())
        bad = [x for x in selected if x not in CLASSES]
        if bad:
            ap.error("unknown class(es): " + ", ".join(bad))
        selected = tuple(x for x in CLASSES if x in set(selected))

    imports = None
    wanted = None
    unmatched: List[int] = []
    if args.imports:
        try:
            imports = load_imports(args.imports)
        except (ImportsError, OSError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
        wanted = set(imports)

    handlers, warnings = discover_handlers(args.root)
    if wanted is not None:
        for nid in sorted(wanted):
            if not any(h.nid == nid for h in handlers):
                unmatched.append(nid)
        handlers = [h for h in handlers if h.nid in wanted]

    checks: List[Check] = []
    for h in handlers:
        lines = LineIndex(h.text)
        comments = collect_comments(h.text, lines)
        checks.extend(extract_checks(h, comments, lines))
    if selected != CLASSES:
        checks = [c for c in checks if c.cls in selected]
    checks.sort(key=lambda c: (c.file, c.line, c.handler))

    report = build_report(args.root, handlers, checks, args.imports, imports,
                          unmatched, warnings)
    md = markdown(report, selected)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
            f.write("\n")
    if args.md:
        with open(args.md, "w", encoding="utf-8") as f:
            f.write(md)
            if not md.endswith("\n"):
                f.write("\n")
    if not args.json and not args.md:
        sys.stdout.write(md)
        if not md.endswith("\n"):
            sys.stdout.write("\n")
    # A report is data. Callers that want a gate can read counts.by_class.none.
    return 0


if __name__ == "__main__":
    sys.exit(main())
