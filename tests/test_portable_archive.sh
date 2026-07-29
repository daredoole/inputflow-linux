#!/usr/bin/env bash
set -euo pipefail

ARCHIVE="${1:?usage: test_portable_archive.sh ARCHIVE}"
CHECKSUM="${ARCHIVE}.sha256"

[[ -f "$ARCHIVE" ]] || { echo "missing archive: $ARCHIVE" >&2; exit 1; }
[[ -f "$CHECKSUM" ]] || { echo "missing checksum: $CHECKSUM" >&2; exit 1; }

ARCHIVE_DIR="$(cd "$(dirname "$ARCHIVE")" && pwd -P)"
ARCHIVE_NAME="$(basename "$ARCHIVE")"
(cd "$ARCHIVE_DIR" && sha256sum --check "$(basename "$CHECKSUM")")

TEST_ROOT="$(mktemp -d)"
cleanup() {
  if [[ -n "$TEST_ROOT" && -d "$TEST_ROOT" && "$TEST_ROOT" != "/" ]]; then
    rm -rf -- "$TEST_ROOT"
  fi
}
trap cleanup EXIT

tar -xzf "$ARCHIVE" -C "$TEST_ROOT"
mapfile -t PACKAGE_ROOTS < <(find "$TEST_ROOT" -mindepth 1 -maxdepth 1 -type d)
[[ "${#PACKAGE_ROOTS[@]}" -eq 1 ]] || {
  echo "archive must contain exactly one package root" >&2
  exit 1
}
PACKAGE_ROOT="${PACKAGE_ROOTS[0]}"

for executable in \
  bin/mwb_client \
  bin/mwb_tray \
  bin/inputflow-controller \
  libexec/inputflow/mwb-desktop-ui.sh \
  libexec/inputflow/scripts/inputflow-diagnostics-bundle.sh; do
  [[ -x "$PACKAGE_ROOT/$executable" ]] || {
    echo "missing executable: $executable" >&2
    exit 1
  }
done

mkdir -p "$TEST_ROOT/home" "$TEST_ROOT/output"
HOME="$TEST_ROOT/home" \
XDG_CONFIG_HOME="$TEST_ROOT/home/config" \
XDG_STATE_HOME="$TEST_ROOT/home/state" \
PATH="$PACKAGE_ROOT/bin:$PATH" \
  "$PACKAGE_ROOT/bin/mwb_client" --help >/dev/null
HOME="$TEST_ROOT/home" \
XDG_CONFIG_HOME="$TEST_ROOT/home/config" \
XDG_STATE_HOME="$TEST_ROOT/home/state" \
PATH="$PACKAGE_ROOT/bin:$PATH" \
  "$PACKAGE_ROOT/libexec/inputflow/scripts/inputflow-diagnostics-bundle.sh" \
  --preview --output "$TEST_ROOT/output" >/dev/null

echo "portable archive smoke test passed"
