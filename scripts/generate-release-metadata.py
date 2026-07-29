#!/usr/bin/env python3
"""Create a CycloneDX SBOM, provenance statement, and publishable artifact set."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import pathlib
import platform
import re
import shutil
import subprocess


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_output(arguments: list[str], cwd: pathlib.Path) -> str:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        check=False,
        capture_output=True,
        text=True,
        timeout=20,
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def project_version(repo: pathlib.Path) -> str:
    cmake = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\([^)]*\bVERSION\s+([0-9.]+)", cmake, re.DOTALL)
    if not match:
        raise SystemExit("Unable to determine project version")
    return match.group(1)


def gradle_components(repo: pathlib.Path) -> list[dict[str, object]]:
    gradle = (repo / "android/app/build.gradle").read_text(encoding="utf-8")
    dependencies: list[dict[str, object]] = []
    pattern = re.compile(
        r'^\s*(?:implementation|testImplementation)\s+["\']'
        r"([^:'\"]+):([^:'\"]+):([^'\"]+)[\"']",
        re.MULTILINE,
    )
    for group, name, version in pattern.findall(gradle):
        dependencies.append(
            {
                "type": "library",
                "group": group,
                "name": name,
                "version": version,
                "purl": f"pkg:maven/{group}/{name}@{version}",
            }
        )
    return dependencies


def native_components(binary: pathlib.Path, repo: pathlib.Path) -> list[dict[str, object]]:
    output = command_output(["ldd", str(binary)], repo)
    components: list[dict[str, object]] = []
    seen: set[str] = set()
    for line in output.splitlines():
        match = re.match(r"\s*([^\s]+)\s+=>\s+(/[^\s]+)", line)
        if not match:
            continue
        name, resolved = match.groups()
        library = pathlib.Path(resolved)
        if name in seen or not library.is_file():
            continue
        seen.add(name)
        components.append(
            {
                "type": "library",
                "name": name,
                "hashes": [{"alg": "SHA-256", "content": sha256(library)}],
                "properties": [
                    {"name": "inputflow:discovery", "value": "runtime-linker"}
                ],
            }
        )
    return components


def copy_artifact(source: pathlib.Path, destination: pathlib.Path) -> pathlib.Path:
    target = destination / source.name
    shutil.copy2(source, target)
    return target


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--android-apk")
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parent.parent
    build_dir = (repo / args.build_dir).resolve()
    release_dir = build_dir / "release"
    release_dir.mkdir(parents=True, exist_ok=True)
    version = project_version(repo)

    archives = sorted(build_dir.glob("inputflow-*.tar.gz"))
    if len(archives) != 1:
        raise SystemExit("Expected exactly one portable archive")
    apk_source = (
        pathlib.Path(args.android_apk).resolve()
        if args.android_apk
        else repo / "android/app/build/outputs/apk/release/app-release-unsigned.apk"
    )
    if not apk_source.is_file():
        raise SystemExit(f"Android release APK is missing: {apk_source}")

    copied = [
        copy_artifact(archives[0], release_dir),
        copy_artifact(apk_source, release_dir),
        copy_artifact(repo / "CHANGELOG.md", release_dir),
    ]
    copied[1].rename(release_dir / f"inputflow-android-{version}-unsigned.apk")
    copied[1] = release_dir / f"inputflow-android-{version}-unsigned.apk"

    timestamp = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()
    native_binary = build_dir / "mwb_client"
    components = gradle_components(repo)
    if native_binary.is_file():
        components.extend(native_components(native_binary, repo))

    sbom_path = release_dir / "inputflow-sbom.cdx.json"
    sbom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {
            "timestamp": timestamp,
            "component": {
                "type": "application",
                "name": "InputFlow",
                "version": version,
            },
        },
        "components": components,
    }
    sbom_path.write_text(json.dumps(sbom, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    commit = command_output(["git", "rev-parse", "HEAD"], repo)
    dirty = bool(command_output(["git", "status", "--porcelain"], repo))
    subject_paths = copied[:2] + [sbom_path]
    provenance_path = release_dir / "inputflow-provenance.json"
    provenance = {
        "_type": "https://in-toto.io/Statement/v1",
        "subject": [
            {
                "name": path.name,
                "digest": {"sha256": sha256(path)},
            }
            for path in subject_paths
        ],
        "predicateType": "https://slsa.dev/provenance/v1",
        "predicate": {
            "buildDefinition": {
                "buildType": "inputflow-local-release-gate/v1",
                "externalParameters": {
                    "version": version,
                    "linuxBuildType": "Release",
                    "androidArtifactSigned": False,
                },
                "internalParameters": {"workingTreeDirty": dirty},
                "resolvedDependencies": (
                    [{"uri": "git+local", "digest": {"gitCommit": commit}}]
                    if commit
                    else []
                ),
            },
            "runDetails": {
                "builder": {"id": "inputflow-local-release-gate"},
                "metadata": {
                    "invocationId": f"{platform.system().lower()}-{commit[:12] or 'unknown'}",
                    "startedOn": timestamp,
                    "finishedOn": timestamp,
                },
            },
        },
    }
    provenance_path.write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    checksum_paths = copied[:2] + [sbom_path, provenance_path]
    checksum_text = "".join(f"{sha256(path)}  {path.name}\n" for path in checksum_paths)
    (release_dir / "SHA256SUMS").write_text(checksum_text, encoding="utf-8")
    print(f"release metadata generated in {release_dir}")
    print("Android APK is unsigned; sign and verify it before publication.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
