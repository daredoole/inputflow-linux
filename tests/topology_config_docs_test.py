#!/usr/bin/env python3

import re
import sys
from pathlib import Path


EDGE_NAMES = {"left", "right", "up", "down"}
WRAP_POLICIES = {"none", "horizontal", "vertical", "both"}
EXPECTED_EXAMPLES = {"aab", "baa", "aba", "stacked", "asymmetric", "wrap-horizontal"}


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def parse_examples(text):
    examples = []
    for match in re.finditer(r"```ini\n(.*?)\n```", text, re.DOTALL):
        block = match.group(1)
        name_match = re.search(r"^\s*#\s*topology-example:\s*([A-Za-z0-9_-]+)\s*$", block, re.MULTILINE)
        if name_match:
            examples.append((name_match.group(1), block))
    return examples


def parse_int(value, context, minimum=None):
    try:
        parsed = int(value)
    except ValueError:
        fail(f"{context} must be an integer")
    if minimum is not None and parsed < minimum:
        fail(f"{context} must be >= {minimum}")
    return parsed


def parse_line_list(value, expected, context):
    parts = [part.strip() for part in value.split(",")]
    if len(parts) != expected or any(part == "" for part in parts):
        fail(f"{context} expects {expected} comma-separated values")
    return parts


def validate_example(name, block):
    machines = set()
    displays = {}
    links = []
    wrap_seen = False

    for line_number, raw_line in enumerate(block.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        if "=" not in line:
            fail(f"{name}: line {line_number} is missing '='")
        key, value = (part.strip() for part in line.split("=", 1))

        if key == "wrap":
            if value not in WRAP_POLICIES:
                fail(f"{name}: line {line_number} wrap is invalid")
            wrap_seen = True
            continue

        if key == "machine":
            machines.add(value)
            continue

        if key == "display":
            display_id, machine_id, x, y, width, height = parse_line_list(value, 6, f"{name}: line {line_number} display")
            displays[display_id] = {
                "machine": machine_id,
                "x": parse_int(x, f"{name}: {display_id}.x"),
                "y": parse_int(y, f"{name}: {display_id}.y"),
                "width": parse_int(width, f"{name}: {display_id}.width", minimum=1),
                "height": parse_int(height, f"{name}: {display_id}.height", minimum=1),
            }
            continue

        if key == "link":
            source_display, exit_edge, target_display, entry_edge = parse_line_list(value, 4, f"{name}: line {line_number} link")
            links.append((source_display, exit_edge, target_display, entry_edge))
            continue

        fail(f"{name}: line {line_number} unknown key {key}")

    if not wrap_seen:
        fail(f"{name}: missing wrap policy")
    if not machines:
        fail(f"{name}: no machines declared")
    if not displays:
        fail(f"{name}: no displays declared")

    for display_id, display in displays.items():
        if display["machine"] not in machines:
            fail(f"{name}: display {display_id} references missing machine {display['machine']}")

    seen_edges = set()
    for source_display, exit_edge, target_display, entry_edge in links:
        if source_display not in displays:
            fail(f"{name}: link source display is missing: {source_display}")
        if target_display not in displays:
            fail(f"{name}: link target display is missing: {target_display}")
        if exit_edge not in EDGE_NAMES:
            fail(f"{name}: link exit edge is invalid: {exit_edge}")
        if entry_edge not in EDGE_NAMES:
            fail(f"{name}: link entry edge is invalid: {entry_edge}")
        edge_key = (source_display, exit_edge)
        if edge_key in seen_edges:
            fail(f"{name}: duplicate explicit link for {source_display}.{exit_edge}")
        seen_edges.add(edge_key)

    displays_by_machine = {}
    for display_id, display in displays.items():
        displays_by_machine.setdefault(display["machine"], []).append((display_id, display))
    for machine_id, machine_displays in displays_by_machine.items():
        for index, (left_id, left) in enumerate(machine_displays):
            for right_id, right in machine_displays[index + 1:]:
                separated = (
                    left["x"] + left["width"] <= right["x"]
                    or right["x"] + right["width"] <= left["x"]
                    or left["y"] + left["height"] <= right["y"]
                    or right["y"] + right["height"] <= left["y"]
                )
                if not separated:
                    fail(f"{name}: displays {left_id} and {right_id} overlap on machine {machine_id}")


def main():
    if len(sys.argv) != 2:
        fail("usage: topology_config_docs_test.py <docs/topology.md>")

    doc_path = Path(sys.argv[1])
    examples = parse_examples(doc_path.read_text(encoding="utf-8"))
    names = {name for name, _ in examples}
    if names != EXPECTED_EXAMPLES:
        fail(f"topology examples mismatch: expected {sorted(EXPECTED_EXAMPLES)}, got {sorted(names)}")

    for name, block in examples:
        validate_example(name, block)

    print(f"Validated {len(examples)} topology doc examples.")


if __name__ == "__main__":
    main()
