#!/usr/bin/env python3
"""Prevent mutable CI dependencies and unsafe trigger regressions."""

from __future__ import annotations

import pathlib
import re
import sys


ACTION_REF = re.compile(r"^[-A-Za-z0-9_.]+/[-A-Za-z0-9_.]+(?:/[-A-Za-z0-9_.]+)?@([0-9a-f]{40})$")
CONTAINER_REF = re.compile(r"^[^\s]+@sha256:[0-9a-f]{64}$")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_ci_security.py WORKFLOW_DIR")
    workflow_dir = pathlib.Path(sys.argv[1])
    failures: list[str] = []
    for path in sorted(workflow_dir.glob("*.y*ml")):
        text = path.read_text(encoding="utf-8")
        if "pull_request_target:" in text:
            failures.append(f"{path}: pull_request_target is forbidden")
        if not re.search(r"^permissions:", text, re.MULTILINE):
            failures.append(f"{path}: explicit top-level permissions are required")
        for line_number, line in enumerate(text.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("uses:"):
                action = stripped.split(":", 1)[1].strip().split(" #", 1)[0]
                if not ACTION_REF.fullmatch(action):
                    failures.append(
                        f"{path}:{line_number}: action must use an immutable 40-character SHA: {action}"
                    )
            if stripped.startswith("image:"):
                image = stripped.split(":", 1)[1].strip()
                if not CONTAINER_REF.fullmatch(image):
                    failures.append(
                        f"{path}:{line_number}: container must use an immutable sha256 digest: {image}"
                    )
    if failures:
        print("CI security contract failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print("CI security contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
