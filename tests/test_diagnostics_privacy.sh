#!/usr/bin/env bash
set -euo pipefail

SCRIPT_PATH="${1:?usage: test_diagnostics_privacy.sh SCRIPT_PATH}"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

CONFIG_PATH="$TEST_ROOT/config.ini"
STATE_PATH="$TEST_ROOT/state.ini"
OUTPUT_PATH="$TEST_ROOT/output"
SECRET_MARKER="InputFlowSecret-MUST-NOT-LEAK-9482"
HOST_MARKER="private-laptop-must-not-leak"
USER_MARKER="private-user-must-not-leak"
IP_MARKER="203.0.113.77"
MAC_MARKER="02:11:22:33:44:55"

mkdir -p "$OUTPUT_PATH"
cat >"$CONFIG_PATH" <<EOF
host=$HOST_MARKER
key=$SECRET_MARKER
username=$USER_MARKER
address=$IP_MARKER
device_mac=$MAC_MARKER
EOF
cat >"$STATE_PATH" <<EOF
peer=$HOST_MARKER,$IP_MARKER
last_device=$MAC_MARKER
token=$SECRET_MARKER
EOF

BUNDLE_PATH="$("$SCRIPT_PATH" \
  --config "$CONFIG_PATH" \
  --state "$STATE_PATH" \
  --output "$OUTPUT_PATH" \
  --preview)"

[[ -d "$BUNDLE_PATH" ]]
[[ -f "$BUNDLE_PATH/consent.json" ]]
[[ ! -e "$BUNDLE_PATH/journal-user-recent.txt" ]]
[[ ! -e "$BUNDLE_PATH/network-hints.txt" ]]

if grep -R -F -e "$SECRET_MARKER" -e "$HOST_MARKER" -e "$USER_MARKER" \
    -e "$IP_MARKER" -e "$MAC_MARKER" "$BUNDLE_PATH"; then
  echo "diagnostics bundle leaked private test data" >&2
  exit 1
fi

grep -F '"automatic_upload": false' "$BUNDLE_PATH/consent.json" >/dev/null
grep -F '"service_journal": false' "$BUNDLE_PATH/consent.json" >/dev/null
grep -F '"network_details": false' "$BUNDLE_PATH/consent.json" >/dev/null
grep -F 'Host: [REDACTED]' "$BUNDLE_PATH/README.txt" >/dev/null
grep -F 'User: [REDACTED]' "$BUNDLE_PATH/README.txt" >/dev/null

while IFS= read -r file; do
  [[ "$(stat -c '%a' "$file")" == "600" ]]
done < <(find "$BUNDLE_PATH" -type f -print)

echo "diagnostics privacy checks passed"
