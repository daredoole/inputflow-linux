#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

if [[ -z "${INPUTFLOW_ANDROID_APK:-}" ]]; then
  echo "production release gate: INPUTFLOW_ANDROID_APK must name the signed APK" >&2
  exit 2
fi

export INPUTFLOW_REQUIRE_SIGNED_ANDROID=1
exec "$REPO_ROOT/scripts/release-gate.sh"
