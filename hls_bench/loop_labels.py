"""Deterministically label synthesizable C loops for HLS staging.

Role:
Provide one shared source-to-source transform that can be used by the different
tool runners when they stage kernel sources into their isolated work trees.

Goal:
Give loops stable, lexicographically ordered labels such as ``L_1_0`` and
``L_1_1`` so later report generation and tool directives can refer to loop nests
without depending on compiler-invented names.

How:
The helper scans C source text line by line and normalizes every loop label to
a deterministic ``L_<nest>_<depth>`` form.  The label numbering is conservative:
each new top-level loop nest gets a new prefix, and nested loops reuse that
prefix with an increasing depth suffix.
"""

from __future__ import annotations

from dataclasses import dataclass
import re
from pathlib import Path


@dataclass(frozen=True)
class LoopLabel:
    """One loop discovered in a C translation unit.

    Role:
    Record the source line, generated or preserved label, and nest coordinates for
    one loop statement.

    Goal:
    Let staging code and downstream report generation use the same deterministic
    loop-name decisions without re-deriving them independently.
    """

    line_index: int
    label: str
    label_line_index: int | None
    nest_id: int
    nest_depth: int
    kind: str
    already_labeled: bool


_LABEL_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(?:$|//|/\*)")
_LABEL_AND_LOOP_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(for|while)\b")
_LOOP_RE = re.compile(r"^\s*(for|while)\b")


def label_c_source_text(source_text: str, *, prefix: str = "L") -> tuple[str, list[LoopLabel]]:
    """Return loop-labeled C source text plus the discovered loop metadata.

    Role:
    Reformat one source translation unit so every loop begins with a stable,
    polyhedral-style label.

    Goal:
    Make the staged kernel sources readable to humans and more predictable for
    tooling that wants deterministic loop identifiers.
    """

    lines = source_text.splitlines(keepends=True)
    entries = _scan_loop_labels(lines, prefix=prefix)
    replacements = {entry.label_line_index: entry.label for entry in entries if entry.label_line_index is not None}
    insertions: dict[int, list[str]] = {}
    for entry in entries:
        if entry.label_line_index is not None:
            continue
        insertion_index = _pragma_insertion_index(lines, entry.line_index)
        insertions.setdefault(insertion_index, []).append(entry.label)
    rewritten: list[str] = []
    for index, line in enumerate(lines):
        for label in insertions.get(index, []):
            indent = re.match(r"^(\s*)", line).group(1)
            rewritten.append(f"{indent}{label}:\n")
        replacement = replacements.get(index)
        if replacement is not None:
            stripped = line.strip()
            same_line = _LABEL_AND_LOOP_RE.match(stripped)
            if same_line is not None:
                indent = re.match(r"^(\s*)", line).group(1)
                tail = line[line.index(":") + 1 :]
                rewritten.append(f"{indent}{replacement}:{tail}")
                continue
            if _LABEL_RE.match(stripped) is not None:
                indent = re.match(r"^(\s*)", line).group(1)
                rewritten.append(f"{indent}{replacement}:\n")
                continue
        rewritten.append(line)
    return "".join(rewritten), entries


def label_c_source_file(path: Path, *, prefix: str = "L") -> list[LoopLabel]:
    """Rewrite one C source file in place with deterministic loop labels.

    Role:
    Apply the shared loop-label transform directly to a staged source file.

    Goal:
    Keep the canonical benchmark tree unchanged while making the per-tool staged
    copy expose stable labels for every loop.
    """

    source_text = path.read_text(encoding="utf-8", errors="replace")
    labeled_text, entries = label_c_source_text(source_text, prefix=prefix)
    if labeled_text != source_text:
        path.write_text(labeled_text, encoding="utf-8")
    return entries


def _scan_loop_labels(lines: list[str], *, prefix: str) -> list[LoopLabel]:
    """Scan a C source file and assign deterministic loop labels.

    Role:
    Walk the source once, tracking braced loop nests so a nested loop can reuse
    its parent nest identifier with a deeper suffix.

    Goal:
    Produce ``L_<nest>_<depth>`` names in lexical source order without depending on
    compiler output or on non-local source rewrites.
    """

    entries: list[LoopLabel] = []
    open_loops: list[tuple[int, int]] = []
    brace_depth = 0
    next_nest_id = 1
    pending_braced_loop: tuple[int, int, str, bool, int | None] | None = None
    next_loop_index: dict[int, int] = {}
    cleaned_lines: list[str] = []
    in_block_comment = False
    for line in lines:
        clean, in_block_comment = _strip_c_comments(line, in_block_comment)
        cleaned_lines.append(clean)

    for index, clean in enumerate(cleaned_lines):
        stripped = clean.strip()
        if pending_braced_loop is not None and stripped.startswith("{"):
            nest_id, nest_depth, label, already_labeled, label_line_index = pending_braced_loop
            open_loops.append((nest_id, brace_depth + clean.count("{")))
            pending_braced_loop = None

        labeled_same_line = _LABEL_AND_LOOP_RE.match(stripped)
        if labeled_same_line is not None:
            kind = labeled_same_line.group(2)
            nest_id = open_loops[-1][0] if open_loops else next_nest_id
            if not open_loops:
                next_nest_id += 1
            loop_index = next_loop_index.get(nest_id, 0)
            next_loop_index[nest_id] = loop_index + 1
            label = f"{prefix}_{nest_id}_{loop_index}"
            entries.append(LoopLabel(index, label, index, nest_id, len(open_loops), kind, True))
            if _line_has_braced_body(clean, lines, index, cleaned_lines):
                pending_braced_loop = (nest_id, len(open_loops), label, True, index)
        elif _LOOP_RE.match(stripped) is not None:
            kind = _LOOP_RE.match(stripped).group(1)
            existing_index = _previous_label_index(cleaned_lines, index)
            if existing_index is not None:
                already_labeled = True
            else:
                already_labeled = False
            nest_id = open_loops[-1][0] if open_loops else next_nest_id
            if not open_loops:
                next_nest_id += 1
            loop_index = next_loop_index.get(nest_id, 0)
            next_loop_index[nest_id] = loop_index + 1
            label = f"{prefix}_{nest_id}_{loop_index}"
            entries.append(LoopLabel(index, label, existing_index, nest_id, len(open_loops), kind, already_labeled))
            if _line_has_braced_body(clean, lines, index, cleaned_lines):
                pending_braced_loop = (nest_id, len(open_loops), label, already_labeled, existing_index)

        brace_depth += clean.count("{")
        brace_depth -= clean.count("}")
        while open_loops and open_loops[-1][1] > brace_depth:
            open_loops.pop()

        if pending_braced_loop is not None and clean.count("{") > 0:
            nest_id, nest_depth, label, already_labeled, label_line_index = pending_braced_loop
            open_loops.append((nest_id, brace_depth))
            pending_braced_loop = None

    return entries


def _previous_label_index(cleaned_lines: list[str], index: int) -> int | None:
    """Return the closest prior explicit source label index before one loop line."""

    for prior in range(index - 1, -1, -1):
        stripped = cleaned_lines[prior].strip()
        if not stripped:
            continue
        match = _LABEL_RE.match(stripped)
        if match is not None:
            return prior
        if _LOOP_RE.match(stripped) is not None:
            break
        if stripped.startswith("#pragma") or stripped.startswith("//") or stripped.startswith("/*"):
            continue
        break
    return None


def _pragma_insertion_index(lines: list[str], loop_index: int) -> int:
    """Return the earliest contiguous HLS pragma line attached to a loop.

    Role:
    Keep generated labels from splitting a pragma and the loop statement it
    controls when staging a source file for HLS tools.

    Goal:
    Preserve compiler pragma attachment while retaining deterministic labels.
    """

    insertion_index = loop_index
    for prior in range(loop_index - 1, -1, -1):
        stripped = lines[prior].strip()
        if not stripped or stripped.startswith("//") or stripped.startswith("/*"):
            continue
        if stripped.startswith("#pragma HLS"):
            insertion_index = prior
            continue
        break
    return insertion_index


def _line_has_braced_body(clean: str, lines: list[str], index: int, cleaned_lines: list[str]) -> bool:
    """Return whether a loop line opens a braced body immediately."""

    if "{" in clean:
        return True
    for next_index in range(index + 1, len(cleaned_lines)):
        stripped = cleaned_lines[next_index].strip()
        if not stripped:
            continue
        if stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("#pragma"):
            continue
        return stripped.startswith("{")
    return False


def _strip_c_comments(line: str, in_block_comment: bool) -> tuple[str, bool]:
    """Remove C comments and strings from one source line.

    Role:
    Keep the loop-label scanner from counting braces that appear inside comments or
    string literals.

    Goal:
    Make the label transform conservative without requiring a full C parser.
    """

    out: list[str] = []
    i = 0
    in_string: str | None = None
    while i < len(line):
        ch = line[i]
        nxt = line[i + 1] if i + 1 < len(line) else ""
        if in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                i += 2
            else:
                i += 1
            continue
        if in_string is not None:
            if ch == "\\":
                i += 2
                continue
            if ch == in_string:
                in_string = None
            i += 1
            continue
        if ch == "/" and nxt == "/":
            break
        if ch == "/" and nxt == "*":
            in_block_comment = True
            i += 2
            continue
        if ch in {'"', "'"}:
            in_string = ch
            i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out), in_block_comment
