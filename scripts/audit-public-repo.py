#!/usr/bin/env python3
"""Reject files and content that should not be published in the repository."""

from __future__ import annotations

import argparse
import ipaddress
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]

SENSITIVE_FILE = re.compile(
    r"(^|/)(?:"
    r"\.env(?:\.|$)|"
    r"id_(?:rsa|ed25519)|"
    r"config\.ini$|"
    r"inputflow-diagnostics-.*\.(?:tar\.gz|zip)$|"
    r".*\.(?:pem|key|p12|pfx|jks|keystore|mobileprovision)$"
    r")",
    re.IGNORECASE,
)
PRIVATE_KEY = re.compile(r"BEGIN (?:[A-Z ]+ )?PRIVATE KEY")
HOME_PATH = re.compile(
    r"(?<![A-Za-z0-9_}])/home/([a-z_][a-z0-9_-]*)",
    re.IGNORECASE,
)
WINDOWS_PROFILE = re.compile(r"[A-Z]:\\Users\\([^\\\s]+)", re.IGNORECASE)
RUNTIME_UID = re.compile(r"/run/user/\d+")
EMAIL = re.compile(
    r"(?<![A-Z0-9._%+-])([A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,})(?![A-Z0-9._%+-])",
    re.IGNORECASE,
)
IPV4 = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
MAC = re.compile(r"\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b", re.IGNORECASE)

PLACEHOLDER_USERS = {"example", "user", "test"}
PLACEHOLDER_EMAIL_DOMAINS = {
    "example.com",
    "example.invalid",
    "users.noreply.github.com",
}
DOCUMENTATION_NETWORKS = tuple(
    ipaddress.ip_network(value)
    for value in (
        "0.0.0.0/8",
        "127.0.0.0/8",
        "192.0.2.0/24",
        "198.51.100.0/24",
        "203.0.113.0/24",
        "224.0.0.0/4",
    )
)


def git_paths(arguments: list[str]) -> list[str]:
    result = subprocess.run(
        ["git", *arguments, "-z"],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
    )
    return [value for value in result.stdout.decode().split("\0") if value]


def publishable_paths(include_untracked: bool) -> list[str]:
    paths = set(git_paths(["ls-files"]))
    if include_untracked:
        paths.update(git_paths(["ls-files", "--others", "--exclude-standard"]))
    return sorted(paths)


def is_disallowed_ip(value: str) -> bool:
    try:
        address = ipaddress.ip_address(value)
    except ValueError:
        return False
    if any(address in network for network in DOCUMENTATION_NETWORKS):
        return False
    return address.is_private or address.is_link_local


def audit_file(relative: str) -> list[str]:
    problems: list[str] = []
    if SENSITIVE_FILE.search(relative):
        problems.append("sensitive filename")

    path = ROOT / relative
    if not path.is_file() or path.is_symlink():
        return problems

    data = path.read_bytes()
    if b"\0" in data:
        return problems
    text = data.decode("utf-8", errors="replace")

    if PRIVATE_KEY.search(text):
        problems.append("private-key material")

    for match in HOME_PATH.finditer(text):
        if match.group(1).lower() not in PLACEHOLDER_USERS:
            problems.append("personal home-directory path")
            break

    for match in WINDOWS_PROFILE.finditer(text):
        if match.group(1).lower() not in PLACEHOLDER_USERS:
            problems.append("personal Windows profile path")
            break

    if RUNTIME_UID.search(text):
        problems.append("hard-coded desktop runtime UID")

    for match in EMAIL.finditer(text):
        domain = match.group(1).rsplit("@", 1)[1].lower()
        if domain not in PLACEHOLDER_EMAIL_DOMAINS:
            problems.append("personal email address")
            break

    if any(is_disallowed_ip(value) for value in IPV4.findall(text)):
        problems.append("private or link-local IP address")

    if not relative.startswith("tests/") and MAC.search(text):
        problems.append("MAC address")

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Audit publishable repository files for secrets and personal/device data."
    )
    parser.add_argument(
        "--include-untracked",
        action="store_true",
        help="also inspect untracked files not excluded by .gitignore",
    )
    args = parser.parse_args()

    findings: list[tuple[str, str]] = []
    paths = publishable_paths(args.include_untracked)
    for relative in paths:
        for problem in audit_file(relative):
            findings.append((relative, problem))

    if findings:
        for relative, problem in findings:
            print(f"{relative}: {problem}", file=sys.stderr)
        print(
            f"public repository audit failed with {len(findings)} finding(s)",
            file=sys.stderr,
        )
        return 1

    scope = "tracked and untracked" if args.include_untracked else "tracked"
    print(f"public repository audit passed: {len(paths)} {scope} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
