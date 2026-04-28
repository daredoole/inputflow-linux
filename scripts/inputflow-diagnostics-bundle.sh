#!/usr/bin/env bash
set -u
set -o pipefail

SCRIPT_NAME="$(basename "$0")"

usage() {
  cat <<EOF
Usage: $SCRIPT_NAME [--config PATH] [--state PATH] [--output DIR]

Create a redacted InputFlow diagnostics bundle.

Options:
  --config PATH  Config file to summarize (default: XDG config mwb-client/config.ini)
  --state PATH   State file to include if present (default: XDG state mwb-client/state.ini)
  --output DIR   Directory where the bundle archive is created (default: current directory)
  -h, --help     Show this help
EOF
}

log() {
  printf '%s\n' "$*" >&2
}

die() {
  log "error: $*"
  exit 1
}

have() {
  command -v "$1" >/dev/null 2>&1
}

default_config_path() {
  if [[ -n "${XDG_CONFIG_HOME:-}" ]]; then
    printf '%s\n' "$XDG_CONFIG_HOME/mwb-client/config.ini"
  elif [[ -n "${HOME:-}" ]]; then
    printf '%s\n' "$HOME/.config/mwb-client/config.ini"
  else
    printf '%s\n' "mwb-client/config.ini"
  fi
}

default_state_path() {
  if [[ -n "${XDG_STATE_HOME:-}" ]]; then
    printf '%s\n' "$XDG_STATE_HOME/mwb-client/state.ini"
  elif [[ -n "${HOME:-}" ]]; then
    printf '%s\n' "$HOME/.local/state/mwb-client/state.ini"
  else
    printf '%s\n' "mwb-client/state.ini"
  fi
}

CONFIG_PATH="$(default_config_path)"
STATE_PATH="$(default_state_path)"
OUTPUT_DIR="."

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)
      [[ $# -ge 2 ]] || die "--config requires a path"
      CONFIG_PATH="$2"
      shift 2
      ;;
    --state)
      [[ $# -ge 2 ]] || die "--state requires a path"
      STATE_PATH="$2"
      shift 2
      ;;
    --output)
      [[ $# -ge 2 ]] || die "--output requires a directory"
      OUTPUT_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument: $1"
      ;;
  esac
done

mkdir -p "$OUTPUT_DIR" || die "failed to create output directory: $OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd -P)" || die "failed to resolve output directory: $OUTPUT_DIR"

TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
BUNDLE_NAME="inputflow-diagnostics-${TIMESTAMP}-$$"
BUNDLE_DIR="$OUTPUT_DIR/$BUNDLE_NAME"
mkdir -p "$BUNDLE_DIR" || die "failed to create bundle directory: $BUNDLE_DIR"
chmod 700 "$BUNDLE_DIR" 2>/dev/null || true

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P 2>/dev/null || pwd -P)"
CONFIG_PATH_DISPLAY="$CONFIG_PATH"
STATE_PATH_DISPLAY="$STATE_PATH"

redact_stream() {
  sed -E \
    -e 's/^([[:space:]]*[^#;[:space:]]*(key|token|secret|password|passphrase)[^=]*[[:space:]]*=[[:space:]]*).*/\1[REDACTED]/I' \
    -e 's/([[:space:]]--?[^[:space:]]*(key|token|secret|password|passphrase)[^[:space:]=]*[=[:space:]]+)[^[:space:]]+/\1[REDACTED]/Ig' \
    -e 's/([A-Za-z_][A-Za-z0-9_]*(KEY|TOKEN|SECRET|PASSWORD|PASSPHRASE)[A-Za-z0-9_]*=)[^[:space:]]+/\1[REDACTED]/Ig' \
    -e "s/((secret|key|token|password|passphrase)( service)? id[[:space:]]+'?)[^'[:space:]]+('?)/\1[REDACTED]\4/Ig" \
    -e 's/(<SECURITY_KEY>|SECURITY_KEY|security key)[^[:space:]]*/\1[REDACTED]/Ig' \
    -e 's/[A-Fa-f0-9]{32,}/[REDACTED_HEX]/g'
}

safe_run() {
  local output_file="$1"
  shift
  {
    printf '$'
    printf ' %q' "$@"
    printf '\n\n'
    "$@"
    local status=$?
    printf '\n[exit status: %s]\n' "$status"
  } 2>&1 | redact_stream >"$output_file"
}

safe_shell() {
  local output_file="$1"
  local description="$2"
  local script="$3"
  {
    printf '$ %s\n\n' "$description"
    bash -c "$script"
    local status=$?
    printf '\n[exit status: %s]\n' "$status"
  } 2>&1 | redact_stream >"$output_file"
}

redacted_copy_or_note() {
  local source_file="$1"
  local output_file="$2"
  {
    printf 'source=%s\n' "$source_file"
    if [[ -f "$source_file" && -r "$source_file" ]]; then
      printf 'present=yes\n\n'
      redact_stream <"$source_file"
    elif [[ -e "$source_file" ]]; then
      printf 'present=yes\nreadable=no\n'
    else
      printf 'present=no\n'
    fi
  } >"$output_file"
}

write_config_summary() {
  local output_file="$1"
  {
    printf 'config_path=%s\n' "$CONFIG_PATH_DISPLAY"
    if [[ ! -e "$CONFIG_PATH" ]]; then
      printf 'present=no\n'
      return
    fi
    printf 'present=yes\n'
    if [[ -f "$CONFIG_PATH" ]]; then
      stat -c 'mode=%A
owner=%U:%G
size_bytes=%s
modified=%y' "$CONFIG_PATH" 2>/dev/null || true
    fi
    if [[ ! -r "$CONFIG_PATH" ]]; then
      printf 'readable=no\n'
      return
    fi
    printf 'readable=yes\n\n[redacted values]\n'
    local line trimmed key value
    while IFS= read -r line || [[ -n "$line" ]]; do
      trimmed="${line#"${line%%[![:space:]]*}"}"
      trimmed="${trimmed%"${trimmed##*[![:space:]]}"}"
      [[ -z "$trimmed" || "$trimmed" == \#* || "$trimmed" == \;* ]] && continue
      if [[ "$trimmed" != *"="* ]]; then
        printf '%s\n' "$trimmed" | redact_stream
        continue
      fi
      key="${trimmed%%=*}"
      value="${trimmed#*=}"
      key="${key%"${key##*[![:space:]]}"}"
      value="${value#"${value%%[![:space:]]*}"}"
      if [[ "$key" =~ [Kk][Ee][Yy]|[Tt][Oo][Kk][Ee][Nn]|[Ss][Ee][Cc][Rr][Ee][Tt]|[Pp][Aa][Ss][Ss] ]]; then
        printf '%s=[REDACTED]\n' "$key"
      else
        printf '%s=%s\n' "$key" "$value" | redact_stream
      fi
    done <"$CONFIG_PATH"
  } >"$output_file"
}

write_state_summary() {
  local output_file="$1"
  {
    printf 'state_path=%s\n' "$STATE_PATH_DISPLAY"
    if [[ ! -e "$STATE_PATH" ]]; then
      printf 'present=no\n'
      return
    fi
    printf 'present=yes\n'
    stat -c 'mode=%A
owner=%U:%G
size_bytes=%s
modified=%y' "$STATE_PATH" 2>/dev/null || true
    if [[ ! -r "$STATE_PATH" ]]; then
      printf 'readable=no\n'
      return
    fi
    printf 'readable=yes\n'
    printf 'peer_lines=%s\n' "$(grep -c '^peer=' "$STATE_PATH" 2>/dev/null || printf '0')"
    printf '\n[redacted state]\n'
    redact_stream <"$STATE_PATH"
  } >"$output_file"
}

write_manifest() {
  cat >"$BUNDLE_DIR/README.txt" <<EOF
InputFlow diagnostics bundle
Created: $(date -Is 2>/dev/null || date)
Host: $(hostname 2>/dev/null || printf 'unknown')
User: $(id -un 2>/dev/null || printf 'unknown')
Repository: $REPO_ROOT

This bundle is redacted by best effort before files are written. Security keys,
tokens, passwords, key file values, secret IDs, and long hex strings are replaced.
Review before sharing outside trusted support channels.
EOF
}

write_manifest
write_config_summary "$BUNDLE_DIR/config-summary.txt"
write_state_summary "$BUNDLE_DIR/app-state.txt"

safe_shell "$BUNDLE_DIR/os-session.txt" "collect OS and session info" '
set +e
date -Is 2>/dev/null || date
uname -a
printf "\n[/etc/os-release]\n"
test -r /etc/os-release && cat /etc/os-release
printf "\n[session]\n"
id
printf "SHELL=%s\nUSER=%s\nLOGNAME=%s\nXDG_SESSION_TYPE=%s\nXDG_CURRENT_DESKTOP=%s\nDESKTOP_SESSION=%s\nDISPLAY=%s\nWAYLAND_DISPLAY=%s\n" "${SHELL:-}" "${USER:-}" "${LOGNAME:-}" "${XDG_SESSION_TYPE:-}" "${XDG_CURRENT_DESKTOP:-}" "${DESKTOP_SESSION:-}" "${DISPLAY:-}" "${WAYLAND_DISPLAY:-}"
if command -v loginctl >/dev/null 2>&1 && test -n "${XDG_SESSION_ID:-}"; then
  loginctl show-session "$XDG_SESSION_ID" --no-pager 2>/dev/null
fi
'

safe_shell "$BUNDLE_DIR/systemd-user-status.txt" "collect systemd user service status" '
set +e
if ! command -v systemctl >/dev/null 2>&1; then
  echo "systemctl not found"
  exit 0
fi
systemctl --user --no-pager status mwb-client.service inputflow.service 2>&1
printf "\n[known matching units]\n"
systemctl --user --no-pager list-units "mwb*" "inputflow*" 2>&1
printf "\n[unit files]\n"
systemctl --user --no-pager list-unit-files "mwb*" "inputflow*" 2>&1
'

safe_shell "$BUNDLE_DIR/journal-user-recent.txt" "collect recent user journal logs" '
set +e
if ! command -v journalctl >/dev/null 2>&1; then
  echo "journalctl not found"
  exit 0
fi
journalctl --user --no-pager --since "2 hours ago" -u mwb-client.service -u inputflow.service -n 300 2>&1
'

safe_shell "$BUNDLE_DIR/uinput-state.txt" "collect uinput state" '
set +e
printf "[device]\n"
ls -l /dev/uinput 2>&1
stat /dev/uinput 2>&1
printf "\n[user groups]\n"
id
printf "\n[modules]\n"
grep "^uinput " /proc/modules 2>/dev/null || true
lsmod 2>/dev/null | grep -E "^uinput|uinput" || true
printf "\n[udev rules]\n"
find /etc/udev/rules.d /usr/lib/udev/rules.d /lib/udev/rules.d -maxdepth 1 \( -iname "*uinput*" -o -iname "*inputflow*" -o -iname "*mwb*" \) -print -exec sh -c '"'"'for f; do echo "--- $f"; sed -n "1,120p" "$f"; done'"'"' sh {} + 2>/dev/null
'

safe_shell "$BUNDLE_DIR/network-hints.txt" "collect network hints" '
set +e
hostnamectl 2>/dev/null || hostname 2>/dev/null
printf "\n[addresses]\n"
ip -brief address 2>&1
printf "\n[links]\n"
ip -brief link 2>&1
printf "\n[routes]\n"
ip route 2>&1
printf "\n[listening/inputflow sockets]\n"
if command -v ss >/dev/null 2>&1; then
  ss -lntup 2>/dev/null | grep -Ei "15101|mwb|inputflow|State|Netid" || true
else
  echo "ss not found"
fi
printf "\n[host resolution]\n"
getent hosts "$(hostname)" 2>/dev/null || true
'

safe_shell "$BUNDLE_DIR/package-build-info.txt" "collect package and build info" "
set +e
printf '[repository]\\n'
if command -v git >/dev/null 2>&1 && git -C '$REPO_ROOT' rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git -C '$REPO_ROOT' rev-parse --show-toplevel
  git -C '$REPO_ROOT' rev-parse --short HEAD
  git -C '$REPO_ROOT' status --short
fi
printf '\\n[build artifacts]\\n'
for f in '$REPO_ROOT/build/mwb_client' '$REPO_ROOT/build/mwb_tray'; do
  if test -e \"\$f\"; then
    ls -l \"\$f\"
    file \"\$f\" 2>/dev/null || true
    \"\$f\" --version 2>&1 || true
  else
    echo \"missing: \$f\"
  fi
done
printf '\\n[cmake cache summary]\\n'
if test -r '$REPO_ROOT/build/CMakeCache.txt'; then
  grep -E '^(CMAKE_BUILD_TYPE|CMAKE_PROJECT_VERSION|MWB_|CMAKE_CXX_COMPILER|CMAKE_CXX_FLAGS)' '$REPO_ROOT/build/CMakeCache.txt' 2>/dev/null || true
fi
printf '\\n[installed packages]\\n'
if command -v dpkg-query >/dev/null 2>&1; then
  dpkg-query -W 'mwb*' 'inputflow*' 2>/dev/null || true
fi
if command -v rpm >/dev/null 2>&1; then
  rpm -qa 2>/dev/null | grep -Ei 'mwb|inputflow' || true
fi
"

if [[ -x "$REPO_ROOT/build/mwb_client" ]]; then
  safe_run "$BUNDLE_DIR/mwb-client-doctor.txt" "$REPO_ROOT/build/mwb_client" doctor --config "$CONFIG_PATH" --state "$STATE_PATH"
elif command -v mwb_client >/dev/null 2>&1; then
  safe_run "$BUNDLE_DIR/mwb-client-doctor.txt" mwb_client doctor --config "$CONFIG_PATH" --state "$STATE_PATH"
else
  printf 'mwb_client executable not found at %s or in PATH\n' "$REPO_ROOT/build/mwb_client" >"$BUNDLE_DIR/mwb-client-doctor.txt"
fi

FINAL_PATH="$BUNDLE_DIR"
if have tar; then
  ARCHIVE_PATH="$OUTPUT_DIR/$BUNDLE_NAME.tar.gz"
  if tar -czf "$ARCHIVE_PATH" -C "$OUTPUT_DIR" "$BUNDLE_NAME" >/dev/null 2>&1; then
    rm -rf "$BUNDLE_DIR"
    FINAL_PATH="$ARCHIVE_PATH"
  else
    log "warning: tar failed; leaving bundle directory unarchived"
  fi
else
  log "warning: tar not found; leaving bundle directory unarchived"
fi

printf '%s\n' "$FINAL_PATH"
