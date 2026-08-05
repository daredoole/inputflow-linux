#!/usr/bin/env python3
"""Enforce the security contract of the packaged user service."""

from __future__ import annotations

import pathlib
import sys


def parse_service(path: pathlib.Path) -> dict[str, list[str]]:
    values: dict[str, list[str]] = {}
    section = ""
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        if "=" not in line:
            raise ValueError(f"invalid unit line: {raw_line}")
        key, value = line.split("=", 1)
        values.setdefault(f"{section}.{key}", []).append(value)
    return values


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_systemd_hardening.py SERVICE")
    values = parse_service(pathlib.Path(sys.argv[1]))
    required = {
        "Service.NoNewPrivileges": "true",
        "Service.UMask": "0077",
        "Service.PrivateTmp": "true",
        "Service.LockPersonality": "true",
        "Service.RestrictSUIDSGID": "true",
        "Service.RestrictRealtime": "true",
        "Service.RestrictNamespaces": "true",
        "Service.SystemCallArchitectures": "native",
        "Service.LimitCORE": "0",
    }
    failures = [
        f"{key} must be {expected}"
        for key, expected in required.items()
        if values.get(key) != [expected]
    ]
    families = set(" ".join(values.get("Service.RestrictAddressFamilies", [])).split())
    expected_families = {"AF_UNIX", "AF_INET", "AF_INET6", "AF_NETLINK"}
    if families != expected_families:
        failures.append(
            "RestrictAddressFamilies must allow only " + " ".join(sorted(expected_families))
        )
    if failures:
        print("systemd hardening contract failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print("systemd hardening contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
