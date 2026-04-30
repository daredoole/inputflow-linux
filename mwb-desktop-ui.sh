#!/usr/bin/env bash
set -euo pipefail

APP_NAME="InputFlow"
SERVICE_NAME="mwb-client.service"
CONFIG_PATH="${XDG_CONFIG_HOME:-$HOME/.config}/mwb-client/config.ini"
STATE_PATH="${XDG_STATE_HOME:-$HOME/.local/state}/mwb-client/state.ini"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AUTO_CONNECT_CONFIG_KEY="${MWB_AUTO_CONNECT_CONFIG_KEY:-auto_connect_enabled}"
RECONNECT_INITIAL_CONFIG_KEY="${MWB_RECONNECT_INITIAL_CONFIG_KEY:-reconnect_initial_backoff_ms}"
RECONNECT_MAX_CONFIG_KEY="${MWB_RECONNECT_MAX_CONFIG_KEY:-reconnect_max_backoff_ms}"
RECONNECT_IDLE_CONFIG_KEY="${MWB_RECONNECT_IDLE_CONFIG_KEY:-reconnect_idle_retry_ms}"
MPRIS_MEDIA_KEYS_CONFIG_KEY="${MWB_MPRIS_MEDIA_KEYS_CONFIG_KEY:-mpris_media_keys_enabled}"
MPRIS_PLAYER_CONFIG_KEY="${MWB_MPRIS_PLAYER_CONFIG_KEY:-mpris_player}"
LATENCY_REPORT_CONFIG_KEY="${MWB_LATENCY_REPORT_CONFIG_KEY:-latency_report}"
TOPOLOGY_ENABLED_CONFIG_KEY="${MWB_TOPOLOGY_ENABLED_CONFIG_KEY:-topology_enabled}"
TOPOLOGY_FILE_CONFIG_KEY="${MWB_TOPOLOGY_FILE_CONFIG_KEY:-topology_file}"
DIAGNOSTICS_BUNDLE_SCRIPT="$SCRIPT_DIR/scripts/inputflow-diagnostics-bundle.sh"
DEFAULT_AUTO_CONNECT_ENABLED="${MWB_DEFAULT_AUTO_CONNECT_ENABLED:-true}"
DEFAULT_RECONNECT_INITIAL_MS="${MWB_DEFAULT_RECONNECT_INITIAL_MS:-1000}"
DEFAULT_RECONNECT_MAX_MS="${MWB_DEFAULT_RECONNECT_MAX_MS:-300000}"
DEFAULT_RECONNECT_IDLE_MS="${MWB_DEFAULT_RECONNECT_IDLE_MS:-900000}"
SECRET_ID_CONFIG_KEY="${MWB_SECRET_ID_CONFIG_KEY:-key_secret_id}"
SECRET_STORE_SET_COMMAND="${MWB_SECRET_STORE_SET_COMMAND:-secret-store}"
SECRET_STORE_CLEAR_COMMAND="${MWB_SECRET_STORE_CLEAR_COMMAND:-secret-clear}"
SECRET_STORE_ID_OPTION="${MWB_SECRET_STORE_ID_OPTION:---secret-id}"
SECRET_STORE_STDIN_OPTION="${MWB_SECRET_STORE_STDIN_OPTION:---stdin}"
SECRET_ID_CONFIG_KEY_ALIASES=("$SECRET_ID_CONFIG_KEY" "secret_id" "key_secret_id" "key_secret" "secret_service_id")

resolve_repo_binary() {
  local binary_name="$1"
  local best_candidate="" best_mtime=-1 candidate mtime
  for candidate in "$SCRIPT_DIR"/build*/"$binary_name"; do
    [[ "$candidate" == *sanitize* ]] && continue
    if [[ -x "$candidate" ]]; then
      mtime="$(stat -c '%Y' "$candidate" 2>/dev/null || printf '0')"
      if (( mtime > best_mtime )); then
        best_candidate="$candidate"
        best_mtime="$mtime"
      fi
    fi
  done

  [[ -n "$best_candidate" ]] && printf '%s\n' "$best_candidate" && return 0

  for candidate in "$SCRIPT_DIR"/build*/"$binary_name"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  command -v "$binary_name" 2>/dev/null || return 1
}

resolve_binary() {
  resolve_repo_binary "mwb_client"
}

resolve_tray_binary() {
  resolve_repo_binary "mwb_tray"
}

resolve_repo_icon() {
  local candidate
  for candidate in \
    "$SCRIPT_DIR/assets/icons/inputflow-desktop.svg" \
    "$SCRIPT_DIR/assets/icons/inputflow-tray.svg"; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

APP_BIN="$(resolve_binary || true)"
TRAY_BIN="$(resolve_tray_binary || true)"
APP_ICON_PATH="$(resolve_repo_icon || true)"

require_ui() {
  if ! command -v zenity >/dev/null 2>&1; then
    printf 'zenity is required for %s desktop UI.\n' "$APP_NAME" >&2
    exit 1
  fi
  if ! python3 -c "import gi; gi.require_version('Gtk', '3.0')" >/dev/null 2>&1; then
    printf 'python3-gi and GTK3 are required for %s desktop UI.\n' "$APP_NAME" >&2
    exit 1
  fi
}

require_client_binary() {
  if [[ -z "${APP_BIN:-}" ]]; then
    zenity --error --text="Could not find a built mwb_client binary. Build the project first."
    return 1
  fi
}

require_tray_binary() {
  if [[ -z "${TRAY_BIN:-}" ]]; then
    zenity --error --text="Could not find a built mwb_tray binary. Build the tray target first."
    return 1
  fi
}

start_tray() {
  require_tray_binary || return 1
  if pgrep -x "mwb_tray" >/dev/null; then
    printf 'Restarting existing InputFlow tray...\n'
    pkill -x "mwb_tray" || true
    # Give the lock file a moment to be released
    sleep 0.5
  fi
  exec "$TRAY_BIN"
}

trim_whitespace() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s\n' "$value"
}

format_yes_no() {
  case "${1:-}" in
    true|TRUE|yes|YES|1) printf 'Yes' ;;
    *) printf 'No' ;;
  esac
}

format_paired_label() {
  if [[ "${1:-}" == "true" ]]; then
    printf 'Paired'
  else
    printf 'Not yet'
  fi
}

format_epoch_label() {
  local epoch="${1:-0}"
  if [[ -z "$epoch" || "$epoch" == "0" ]]; then
    printf 'Never'
    return 0
  fi

  date -d "@$epoch" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || printf '%s' "$epoch"
}

strip_matching_quotes() {
  local value="$1"
  if [[ "${#value}" -ge 2 ]]; then
    local first_char="${value:0:1}"
    local last_char="${value: -1}"
    if [[ "$first_char" == "$last_char" && ( "$first_char" == '"' || "$first_char" == "'" ) ]]; then
      value="${value:1:${#value}-2}"
    fi
  fi
  printf '%s\n' "$value"
}

config_has_key() {
  local wanted_key="$1"
  local line line_key

  [[ -f "$CONFIG_PATH" ]] || return 1

  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ "$line" =~ ^[[:space:]]*([A-Za-z0-9_.-]+)[[:space:]]*= ]] || continue
    line_key="${BASH_REMATCH[1]}"
    [[ "$line_key" == "$wanted_key" ]] && return 0
  done <"$CONFIG_PATH"

  return 1
}

read_config_value() {
  local key="$1"
  local line line_key line_value value=""

  [[ -f "$CONFIG_PATH" ]] || return 0

  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ "$line" =~ ^[[:space:]]*([A-Za-z0-9_.-]+)[[:space:]]*=(.*)$ ]] || continue
    line_key="${BASH_REMATCH[1]}"
    [[ "$line_key" == "$key" ]] || continue
    line_value="${BASH_REMATCH[2]}"
    line_value="$(trim_whitespace "$line_value")"
    value="$(strip_matching_quotes "$line_value")"
  done <"$CONFIG_PATH"

  printf '%s\n' "$value"
}

read_secret_id_value() {
  local key_name
  for key_name in "${SECRET_ID_CONFIG_KEY_ALIASES[@]}"; do
    if config_has_key "$key_name"; then
      read_config_value "$key_name"
      return 0
    fi
  done
  return 0
}

detect_secret_id_key_name() {
  local key_name
  for key_name in "${SECRET_ID_CONFIG_KEY_ALIASES[@]}"; do
    if config_has_key "$key_name"; then
      printf '%s\n' "$key_name"
      return 0
    fi
  done
  printf '%s\n' "$SECRET_ID_CONFIG_KEY"
}

canonical_managed_key() {
  local input_key="$1"
  local secret_key_name="$2"
  local candidate

  case "$input_key" in
    host|key|key_file|machine_name|port|screen_width|screen_height|clipboard_enabled|clipboard_send_enabled|clipboard_force_poll|clipboard_poll_ms|"$MPRIS_MEDIA_KEYS_CONFIG_KEY"|"$MPRIS_PLAYER_CONFIG_KEY"|"$LATENCY_REPORT_CONFIG_KEY"|"$AUTO_CONNECT_CONFIG_KEY"|"$RECONNECT_INITIAL_CONFIG_KEY"|"$RECONNECT_MAX_CONFIG_KEY"|"$RECONNECT_IDLE_CONFIG_KEY")
      printf '%s\n' "$input_key"
      return 0
      ;;
  esac

  for candidate in "${SECRET_ID_CONFIG_KEY_ALIASES[@]}"; do
    if [[ "$input_key" == "$candidate" ]]; then
      printf '%s\n' "$secret_key_name"
      return 0
    fi
  done

  return 1
}

write_topology_config_keys() {
  local topology_file="$1"
  local tmp_path line line_key
  local saw_enabled=false saw_file=false

  mkdir -p "$(dirname "$CONFIG_PATH")"
  tmp_path="$(mktemp "${CONFIG_PATH}.tmp.XXXXXX")"

  if [[ -f "$CONFIG_PATH" ]]; then
    while IFS= read -r line || [[ -n "$line" ]]; do
      if [[ "$line" =~ ^[[:space:]]*([A-Za-z0-9_.-]+)[[:space:]]*= ]]; then
        line_key="${BASH_REMATCH[1]}"
        case "$line_key" in
          "$TOPOLOGY_ENABLED_CONFIG_KEY")
            printf '%s=true\n' "$TOPOLOGY_ENABLED_CONFIG_KEY" >>"$tmp_path"
            saw_enabled=true
            continue
            ;;
          "$TOPOLOGY_FILE_CONFIG_KEY")
            printf '%s=%s\n' "$TOPOLOGY_FILE_CONFIG_KEY" "$topology_file" >>"$tmp_path"
            saw_file=true
            continue
            ;;
        esac
      fi
      printf '%s\n' "$line" >>"$tmp_path"
    done <"$CONFIG_PATH"
  fi

  [[ "$saw_enabled" == true ]] || printf '%s=true\n' "$TOPOLOGY_ENABLED_CONFIG_KEY" >>"$tmp_path"
  [[ "$saw_file" == true ]] || printf '%s=%s\n' "$TOPOLOGY_FILE_CONFIG_KEY" "$topology_file" >>"$tmp_path"
  mv "$tmp_path" "$CONFIG_PATH"
}

disable_topology_config() {
  local tmp_path line line_key
  local saw_enabled=false

  mkdir -p "$(dirname "$CONFIG_PATH")"
  tmp_path="$(mktemp "${CONFIG_PATH}.tmp.XXXXXX")"

  if [[ -f "$CONFIG_PATH" ]]; then
    while IFS= read -r line || [[ -n "$line" ]]; do
      if [[ "$line" =~ ^[[:space:]]*([A-Za-z0-9_.-]+)[[:space:]]*= ]]; then
        line_key="${BASH_REMATCH[1]}"
        if [[ "$line_key" == "$TOPOLOGY_ENABLED_CONFIG_KEY" ]]; then
          printf '%s=false\n' "$TOPOLOGY_ENABLED_CONFIG_KEY" >>"$tmp_path"
          saw_enabled=true
          continue
        fi
      fi
      printf '%s\n' "$line" >>"$tmp_path"
    done <"$CONFIG_PATH"
  fi

  [[ "$saw_enabled" == true ]] || printf '%s=false\n' "$TOPOLOGY_ENABLED_CONFIG_KEY" >>"$tmp_path"
  mv "$tmp_path" "$CONFIG_PATH"
}

write_config() {
  local host="$1" key="$2" key_file="$3" secret_id="$4" machine_name="$5" port="$6" auto_connect_enabled="$7" reconnect_initial_backoff_ms="$8" reconnect_max_backoff_ms="$9" reconnect_idle_retry_ms="${10}" clipboard_enabled="${11}" clipboard_send_enabled="${12}" clipboard_force_poll="${13}" clipboard_poll_ms="${14}" screen_width="${15}" screen_height="${16}" mpris_media_keys_enabled="${17}" mpris_player="${18}" latency_report="${19}"
  local secret_key_name="${20:-$(detect_secret_id_key_name)}"
  local tmp_path line existing_key managed_key
  local -a existing_lines=()
  local -a ordered_keys=("host" "key" "key_file" "$secret_key_name" "machine_name" "port" "screen_width" "screen_height" "$AUTO_CONNECT_CONFIG_KEY" "$RECONNECT_INITIAL_CONFIG_KEY" "$RECONNECT_MAX_CONFIG_KEY" "$RECONNECT_IDLE_CONFIG_KEY" "clipboard_enabled" "clipboard_send_enabled" "clipboard_force_poll" "clipboard_poll_ms" "$MPRIS_MEDIA_KEYS_CONFIG_KEY" "$MPRIS_PLAYER_CONFIG_KEY" "$LATENCY_REPORT_CONFIG_KEY")
  local -A values=(
    [host]="$host"
    [key]="$key"
    [key_file]="$key_file"
    ["$secret_key_name"]="$secret_id"
    [machine_name]="$machine_name"
    [port]="$port"
    [screen_width]="$screen_width"
    [screen_height]="$screen_height"
    ["$AUTO_CONNECT_CONFIG_KEY"]="$auto_connect_enabled"
    ["$RECONNECT_INITIAL_CONFIG_KEY"]="$reconnect_initial_backoff_ms"
    ["$RECONNECT_MAX_CONFIG_KEY"]="$reconnect_max_backoff_ms"
    ["$RECONNECT_IDLE_CONFIG_KEY"]="$reconnect_idle_retry_ms"
    [clipboard_enabled]="$clipboard_enabled"
    [clipboard_send_enabled]="$clipboard_send_enabled"
    [clipboard_force_poll]="$clipboard_force_poll"
    [clipboard_poll_ms]="$clipboard_poll_ms"
    ["$MPRIS_MEDIA_KEYS_CONFIG_KEY"]="$mpris_media_keys_enabled"
    ["$MPRIS_PLAYER_CONFIG_KEY"]="$mpris_player"
    ["$LATENCY_REPORT_CONFIG_KEY"]="$latency_report"
  )
  local -A seen=()

  mkdir -p "$(dirname "$CONFIG_PATH")"
  tmp_path="$(mktemp "${CONFIG_PATH}.tmp.XXXXXX")"

  if [[ -f "$CONFIG_PATH" ]]; then
    mapfile -t existing_lines <"$CONFIG_PATH"
  fi

  for line in "${existing_lines[@]}"; do
    if [[ "$line" =~ ^[[:space:]]*([A-Za-z0-9_.-]+)[[:space:]]*= ]]; then
      existing_key="${BASH_REMATCH[1]}"
      if managed_key="$(canonical_managed_key "$existing_key" "$secret_key_name")"; then
        if [[ -z "${seen[$managed_key]+x}" ]]; then
          printf '%s=%s\n' "$managed_key" "${values[$managed_key]}" >>"$tmp_path"
          seen["$managed_key"]=1
        fi
        continue
      fi
    fi

    printf '%s\n' "$line" >>"$tmp_path"
  done

  for managed_key in "${ordered_keys[@]}"; do
    if [[ -z "${seen[$managed_key]+x}" ]]; then
      printf '%s=%s\n' "$managed_key" "${values[$managed_key]}" >>"$tmp_path"
    fi
  done

  mv "$tmp_path" "$CONFIG_PATH"
}

set_configured_host() {
  local new_host="$1"
  local current_host key key_file secret_id secret_key_name machine_name port auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms clipboard_enabled clipboard_send_enabled clipboard_force_poll clipboard_poll_ms screen_width screen_height mpris_media_keys_enabled mpris_player latency_report

  current_host="$(read_config_value host)"
  if [[ "$new_host" == "$current_host" ]]; then
    zenity --info --text="$new_host is already the configured Windows host."
    return 0
  fi

  key="$(read_config_value key)"
  key_file="$(read_config_value key_file)"
  secret_id="$(read_secret_id_value)"
  secret_key_name="$(detect_secret_id_key_name)"
  machine_name="$(read_config_value machine_name)"
  port="$(read_config_value port)"; [[ -n "$port" ]] || port="15101"
  IFS=$'\t' read -r auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms < <(read_connection_behavior_values)
  clipboard_enabled="$(read_config_value clipboard_enabled)"; [[ -n "$clipboard_enabled" ]] || clipboard_enabled="true"
  clipboard_send_enabled="$(read_config_value clipboard_send_enabled)"; [[ -n "$clipboard_send_enabled" ]] || clipboard_send_enabled="true"
  clipboard_force_poll="$(read_config_value clipboard_force_poll)"; [[ -n "$clipboard_force_poll" ]] || clipboard_force_poll="false"
  clipboard_poll_ms="$(read_config_value clipboard_poll_ms)"; [[ -n "$clipboard_poll_ms" ]] || clipboard_poll_ms="1000"
  screen_width="$(read_config_value screen_width)"
  screen_height="$(read_config_value screen_height)"
  mpris_media_keys_enabled="$(read_config_value "$MPRIS_MEDIA_KEYS_CONFIG_KEY")"; [[ -n "$mpris_media_keys_enabled" ]] || mpris_media_keys_enabled="true"
  mpris_player="$(read_config_value "$MPRIS_PLAYER_CONFIG_KEY")"
  latency_report="$(read_config_value "$LATENCY_REPORT_CONFIG_KEY")"; [[ -n "$latency_report" ]] || latency_report="false"

  write_config "$new_host" "$key" "$key_file" "$secret_id" "$machine_name" "$port" "$auto_connect_enabled" "$reconnect_initial_backoff_ms" "$reconnect_max_backoff_ms" "$reconnect_idle_retry_ms" "$clipboard_enabled" "$clipboard_send_enabled" "$clipboard_force_poll" "$clipboard_poll_ms" "$screen_width" "$screen_height" "$mpris_media_keys_enabled" "$mpris_player" "$latency_report" "$secret_key_name"
  zenity --info --text="Configured Windows host updated to $new_host."
  offer_service_restart_if_active "The configured Windows host changed while the background service is running."
}

remove_peer_state() {
  local target_host="$1" target_port="$2"
  local tmp_path line host port

  [[ -f "$STATE_PATH" ]] || return 0

  mkdir -p "$(dirname "$STATE_PATH")"
  tmp_path="$(mktemp "${STATE_PATH}.tmp.XXXXXX")"
  while IFS= read -r line || [[ -n "$line" ]]; do
    if [[ "$line" == peer=* ]]; then
      IFS=$'\t' read -r host _name port _rest <<<"${line#peer=}"
      if [[ "$host" == "$target_host" && "$port" == "$target_port" ]]; then
        continue
      fi
    fi
    printf '%s\n' "$line" >>"$tmp_path"
  done <"$STATE_PATH"

  mv "$tmp_path" "$STATE_PATH"
}

read_connection_behavior_values() {
  local auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms

  auto_connect_enabled="$(read_config_value "$AUTO_CONNECT_CONFIG_KEY")"
  reconnect_initial_backoff_ms="$(read_config_value "$RECONNECT_INITIAL_CONFIG_KEY")"
  reconnect_max_backoff_ms="$(read_config_value "$RECONNECT_MAX_CONFIG_KEY")"
  reconnect_idle_retry_ms="$(read_config_value "$RECONNECT_IDLE_CONFIG_KEY")"

  [[ -n "$auto_connect_enabled" ]] || auto_connect_enabled="$DEFAULT_AUTO_CONNECT_ENABLED"
  [[ -n "$reconnect_initial_backoff_ms" ]] || reconnect_initial_backoff_ms="$DEFAULT_RECONNECT_INITIAL_MS"
  [[ -n "$reconnect_max_backoff_ms" ]] || reconnect_max_backoff_ms="$DEFAULT_RECONNECT_MAX_MS"
  [[ -n "$reconnect_idle_retry_ms" ]] || reconnect_idle_retry_ms="$DEFAULT_RECONNECT_IDLE_MS"

  printf '%s\t%s\t%s\t%s\n' \
    "$auto_connect_enabled" \
    "$reconnect_initial_backoff_ms" \
    "$reconnect_max_backoff_ms" \
    "$reconnect_idle_retry_ms"
}

connection_behavior_mode_label() {
  local auto_connect_enabled="$1"

  if [[ "$auto_connect_enabled" == "true" ]]; then
    printf 'Auto-connect to configured host'
    return 0
  fi

  printf 'Manual start only'
}

connection_behavior_summary() {
  local auto_connect_enabled="$1" reconnect_initial_backoff_ms="$2" reconnect_max_backoff_ms="$3" reconnect_idle_retry_ms="$4"
  printf '%s\nRetry: %s ms to %s ms\nOffline idle retry: %s ms' \
    "$(connection_behavior_mode_label "$auto_connect_enabled")" \
    "$reconnect_initial_backoff_ms" \
    "$reconnect_max_backoff_ms" \
    "$reconnect_idle_retry_ms"
}

configured_auth_source_count() {
  local key="$1" key_file="$2" secret_id="$3"
  local count=0

  [[ -n "$key" ]] && (( count += 1 ))
  [[ -n "$key_file" ]] && (( count += 1 ))
  [[ -n "$secret_id" ]] && (( count += 1 ))

  printf '%s\n' "$count"
}

configured_auth_mode() {
  local key="$1" key_file="$2" secret_id="$3"

  if [[ -n "$secret_id" ]]; then
    printf 'Secret Service entry\n'
    return 0
  fi

  if [[ -n "$key_file" ]]; then
    printf 'Key file path\n'
    return 0
  fi

  printf 'Inline security key\n'
}

configured_auth_label() {
  local key="$1" key_file="$2" secret_id="$3"
  local count

  count="$(configured_auth_source_count "$key" "$key_file" "$secret_id")"
  if (( count > 1 )); then
    printf 'Conflicting key sources'
    return 0
  fi

  if [[ -n "$secret_id" ]]; then
    printf 'Secret Service'
    return 0
  fi

  if [[ -n "$key_file" ]]; then
    printf 'Key file'
    return 0
  fi

  if [[ -n "$key" ]]; then
    printf 'Inline key'
    return 0
  fi

  printf 'Not configured'
}

is_integer_in_range() {
  local value="$1" min_value="$2" max_value="$3"
  [[ "$value" =~ ^[0-9]+$ ]] && (( value >= min_value && value <= max_value ))
}

read_peer_state() {
  local wanted_host="$1" wanted_port="$2"
  local line host name port approved connected_now last_seen last_connected

  [[ -f "$STATE_PATH" ]] || return 1

  while IFS= read -r line; do
    [[ "$line" == peer=* ]] || continue
    IFS=$'\t' read -r host name port approved connected_now last_seen last_connected <<<"${line#peer=}"
    [[ "$host" == "$wanted_host" && "$port" == "$wanted_port" ]] || continue
    if [[ -z "$last_connected" ]]; then
      last_connected="${last_seen:-0}"
      last_seen="${connected_now:-0}"
      connected_now="false"
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' "${name:-unknown}" "${approved:-false}" "${connected_now:-false}" "${last_seen:-0}" "${last_connected:-0}"
    return 0
  done <"$STATE_PATH"

  return 1
}

normalize_host_label() {
  local value="$1"
  value="$(trim_value "$value")"
  value="${value%%.*}"
  printf '%s\n' "$value" | tr '[:upper:]' '[:lower:]'
}

host_labels_match() {
  local left right
  left="$(normalize_host_label "$1")"
  right="$(normalize_host_label "$2")"
  [[ -n "$left" && "$left" == "$right" ]]
}

read_peer_state_by_verified_name() {
  local wanted_name="$1" wanted_port="$2"
  local line host name port approved connected_now last_seen last_connected
  local best_name="" best_connected="false" best_last_seen="0" best_last_connected="0" found="false"

  [[ -n "$(normalize_host_label "$wanted_name")" ]] || return 1
  [[ -f "$STATE_PATH" ]] || return 1

  while IFS= read -r line; do
    [[ "$line" == peer=* ]] || continue
    IFS=$'\t' read -r host name port approved connected_now last_seen last_connected <<<"${line#peer=}"
    [[ "$port" == "$wanted_port" && "$approved" == "true" ]] || continue
    host_labels_match "$name" "$wanted_name" || continue
    if [[ -z "$last_connected" ]]; then
      last_connected="${last_seen:-0}"
      last_seen="${connected_now:-0}"
      connected_now="false"
    fi
    [[ "$last_seen" =~ ^[0-9]+$ ]] || last_seen="0"
    [[ "$last_connected" =~ ^[0-9]+$ ]] || last_connected="0"
    if [[ "$found" != "true" || "$last_connected" -gt "$best_last_connected" ]]; then
      best_name="${name:-$wanted_name}"
      best_last_seen="$last_seen"
      best_last_connected="$last_connected"
      found="true"
    fi
    if [[ "$connected_now" == "true" ]]; then
      best_connected="true"
    fi
  done <"$STATE_PATH"

  [[ "$found" == "true" ]] || return 1
  printf '%s\ttrue\t%s\t%s\t%s\n' "$best_name" "$best_connected" "$best_last_seen" "$best_last_connected"
}

resolve_config_relative_path() {
  local path_value="$1"

  if [[ "$path_value" = /* ]]; then
    printf '%s\n' "$path_value"
    return 0
  fi

  printf '%s\n' "$(dirname "$CONFIG_PATH")/$path_value"
}

store_secret_service_key() {
  local secret_id="$1" secret_value="$2"
  local output error_text
  local -a command=("$APP_BIN" "$SECRET_STORE_SET_COMMAND" "$SECRET_STORE_ID_OPTION" "$secret_id")

  require_client_binary || return 1

  if [[ -n "$SECRET_STORE_STDIN_OPTION" ]]; then
    command+=("$SECRET_STORE_STDIN_OPTION")
  fi

  if output="$(printf '%s' "$secret_value" | "${command[@]}" 2>&1)"; then
    return 0
  fi

  printf -v error_text "Failed to store the Secret Service key for identifier '%s'.\n\n%s" "$secret_id" "$output"
  zenity --error --width=700 --text="$error_text"
  return 1
}

clear_secret_service_key() {
  local secret_id="$1"
  local output error_text

  require_client_binary || return 1

  if output="$("$APP_BIN" "$SECRET_STORE_CLEAR_COMMAND" "$SECRET_STORE_ID_OPTION" "$secret_id" 2>&1)"; then
    return 0
  fi

  printf -v error_text "Saved settings, but failed to clear the previous Secret Service entry '%s'.\n\n%s" "$secret_id" "$output"
  zenity --error --width=700 --text="$error_text"
  return 1
}

read_key_file_value() {
  local key_file="$1"
  local resolved_path key_value

  [[ -n "$key_file" ]] || return 1

  resolved_path="$(resolve_config_relative_path "$key_file")"
  if [[ ! -r "$resolved_path" ]]; then
    zenity --error --width=700 --text="Cannot read the configured key file.\n\n$resolved_path"
    return 1
  fi

  key_value="$(<"$resolved_path")"
  if [[ -z "$key_value" ]]; then
    zenity --error --width=700 --text="The configured key file is empty.\n\n$resolved_path"
    return 1
  fi

  printf '%s\n' "$key_value"
}

read_secret_service_key() {
  local secret_id="$1"
  local secret_tool key_value

  [[ -n "$secret_id" ]] || return 1

  secret_tool="$(command -v secret-tool || true)"
  if [[ -z "$secret_tool" ]]; then
    zenity --error --width=700 --text="secret-tool is not available, so the stored Secret Service key cannot be revealed."
    return 1
  fi

  if ! key_value="$("$secret_tool" lookup application mwb-client-linux secret-id "$secret_id" 2>/dev/null)" || [[ -z "$key_value" ]]; then
    zenity --error --width=700 --text="Could not read the Secret Service entry '$secret_id'."
    return 1
  fi

  printf '%s\n' "$key_value"
}

show_current_security_key() {
  local key="$1" key_file="$2" secret_id="$3" current_auth_mode="$4"
  local revealed_key="" detail_text=""

  case "$current_auth_mode" in
    "Inline security key")
      if [[ -z "$key" ]]; then
        zenity --error --text="No inline security key is currently configured."
        return 1
      fi
      revealed_key="$key"
      detail_text="Source: Inline security key"
      ;;
    "Key file path")
      revealed_key="$(read_key_file_value "$key_file")" || return 1
      detail_text="Source: Key file path\nPath: $(resolve_config_relative_path "$key_file")"
      ;;
    "Secret Service entry")
      revealed_key="$(read_secret_service_key "$secret_id")" || return 1
      detail_text="Source: Secret Service entry\nIdentifier: $secret_id"
      ;;
    *)
      zenity --error --text="No security key is currently configured."
      return 1
      ;;
  esac

  if ! zenity --question --title="$APP_NAME authentication" --width=520 \
    --text="Reveal the current security key?\n\nAnyone looking at this screen will be able to read it."; then
    return 0
  fi

  zenity --entry --title="$APP_NAME current security key" --width=640 \
    --text="$detail_text\n\nCopy the current security key below." \
    --entry-text="$revealed_key" >/dev/null || true
}

choose_secret_cleanup_target() {
  local secret_id="$1" next_mode="$2" next_secret_id="$3"
  local choice

  [[ -n "$secret_id" ]] || return 1
  if [[ "$next_mode" == "Secret Service entry" && "$next_secret_id" == "$secret_id" ]]; then
    return 1
  fi

  choice="$(zenity --list --radiolist --title="$APP_NAME secret cleanup" --width=560 --height=220 \
    --text="Authentication will no longer use Secret Service identifier '$secret_id'. Choose what to do with the stored key." \
    --column="Use" --column="Action" \
    TRUE "Keep the stored Secret Service key" \
    FALSE "Clear the stored Secret Service key" || true)"

  if [[ "$choice" == "Clear the stored Secret Service key" ]]; then
    printf '%s\n' "$secret_id"
    return 0
  fi

  return 1
}

service_exists() {
  systemctl --user cat "$SERVICE_NAME" >/dev/null 2>&1
}

service_active() {
  systemctl --user is-active --quiet "$SERVICE_NAME"
}

offer_service_restart_if_active() {
  local restart_reason="$1"

  if ! service_active; then
    return 0
  fi

  if zenity --question --title="$APP_NAME" --width=480 \
    --text="$restart_reason\n\nRestart the background service now to apply the updated settings?"; then
    systemctl --user restart "$SERVICE_NAME" >/dev/null
    zenity --info --text="Restarted the $APP_NAME background service."
  fi
}

install_service() {
  require_client_binary || return 1
  "$APP_BIN" install-user-service --config "$CONFIG_PATH" --force >/dev/null
  systemctl --user daemon-reload
}

service_state() {
  systemctl --user is-active "$SERVICE_NAME" 2>/dev/null || printf 'unknown'
}

service_state_label() {
  case "$1" in
    active) printf 'Running' ;;
    activating) printf 'Starting' ;;
    deactivating) printf 'Stopping' ;;
    reloading) printf 'Restarting' ;;
    failed) printf 'Needs attention' ;;
    inactive) printf 'Stopped' ;;
    *) printf 'Unavailable' ;;
  esac
}

menu_summary_text() {
  local state host key key_file secret_id auth_label auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms topology_enabled topology_file topology_label
  state="$(service_state)"
  host="$(read_config_value host)"
  key="$(read_config_value key)"
  key_file="$(read_config_value key_file)"
  secret_id="$(read_secret_id_value)"
  IFS=$'\t' read -r auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms < <(read_connection_behavior_values)
  auth_label="$(configured_auth_label "$key" "$key_file" "$secret_id")"
  topology_enabled="$(read_config_value "$TOPOLOGY_ENABLED_CONFIG_KEY")"
  topology_file="$(read_config_value "$TOPOLOGY_FILE_CONFIG_KEY")"

  [[ -n "$host" ]] || host="None"
  if [[ "$topology_enabled" == "true" && -n "$topology_file" ]]; then
    topology_label="$(basename "$topology_file")"
  else
    topology_label="Disabled"
  fi

  printf 'Status: %s\nHost: %s\nAuth: %s\nReconnect: %s\nTopology: %s' \
    "$(service_state_label "$state")" \
    "$host" \
    "$auth_label" \
    "$( [[ "$auto_connect_enabled" == "true" ]] && printf 'Auto' || printf 'Manual' )" \
    "$topology_label"
}

show_status() {
  local status_text doctor_text
  status_text="$(systemctl --user status --no-pager "$SERVICE_NAME" 2>&1 || true)"
  doctor_text="$("$APP_BIN" doctor --config "$CONFIG_PATH" --state "$STATE_PATH" 2>&1 || true)"
  zenity --text-info --title="$APP_NAME service status" --width=900 --height=600 <<<"$doctor_text

----

$status_text"
}

append_check_line() {
  local status="$1" name="$2" detail="$3"
  printf '%-5s %-28s %s\n' "$status" "$name" "$detail"
}

probe_tcp_port() {
  local host="$1" port="$2"
  MWB_PROBE_HOST="$host" MWB_PROBE_PORT="$port" timeout 2 bash -c ':</dev/tcp/$MWB_PROBE_HOST/$MWB_PROBE_PORT' >/dev/null 2>&1
}

health_check() {
  require_client_binary || return 1
  local host port key key_file secret_id auth_count service_status health_text doctor_text
  host="$(read_config_value host)"
  port="$(read_config_value port)"; [[ -n "$port" ]] || port="15101"
  key="$(read_config_value key)"
  key_file="$(read_config_value key_file)"
  secret_id="$(read_secret_id_value)"
  auth_count="$(configured_auth_source_count "$key" "$key_file" "$secret_id")"
  service_status="$(service_state)"

  health_text="$(
    append_check_line "$([[ -f "$CONFIG_PATH" ]] && printf OK || printf WARN)" "config file" "$CONFIG_PATH"
    append_check_line "$([[ -e /dev/uinput ]] && printf OK || printf WARN)" "uinput device" "$([[ -e /dev/uinput ]] && ls -l /dev/uinput 2>/dev/null || printf 'missing; install udev rule and reload')"
    append_check_line "$([[ "$service_status" == "active" ]] && printf OK || printf WARN)" "user service" "$(service_state_label "$service_status")"
    append_check_line "$([[ -n "$host" ]] && printf OK || printf WARN)" "Windows host" "${host:-not configured}"
    append_check_line "$([[ "$auth_count" == "1" ]] && printf OK || printf WARN)" "authentication" "$(configured_auth_label "$key" "$key_file" "$secret_id")"
    if [[ -n "$host" ]] && command -v timeout >/dev/null 2>&1; then
      if probe_tcp_port "$host" "$port"; then
        append_check_line OK "input port" "$host:$port reachable"
      else
        append_check_line WARN "input port" "$host:$port not reachable"
      fi
      if probe_tcp_port "$host" "15100"; then
        append_check_line OK "clipboard port" "$host:15100 reachable"
      else
        append_check_line WARN "clipboard port" "$host:15100 not reachable"
      fi
    else
      append_check_line WARN "port probe" "host or timeout command unavailable"
    fi
  )"
  doctor_text="$("$APP_BIN" doctor --config "$CONFIG_PATH" --state "$STATE_PATH" 2>&1 || true)"
  zenity --text-info --title="$APP_NAME health check" --width=900 --height=620 <<<"$health_text

----
Client doctor
----
$doctor_text"
}

connection_quality() {
  local host port state auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms
  local clipboard_enabled clipboard_send_enabled clipboard_force_poll clipboard_poll_ms latency_report quality_text peer_lines
  host="$(read_config_value host)"
  port="$(read_config_value port)"; [[ -n "$port" ]] || port="15101"
  state="$(service_state)"
  IFS=$'\t' read -r auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms < <(read_connection_behavior_values)
  clipboard_enabled="$(read_config_value clipboard_enabled)"; [[ -n "$clipboard_enabled" ]] || clipboard_enabled="true"
  clipboard_send_enabled="$(read_config_value clipboard_send_enabled)"; [[ -n "$clipboard_send_enabled" ]] || clipboard_send_enabled="true"
  clipboard_force_poll="$(read_config_value clipboard_force_poll)"; [[ -n "$clipboard_force_poll" ]] || clipboard_force_poll="false"
  clipboard_poll_ms="$(read_config_value clipboard_poll_ms)"; [[ -n "$clipboard_poll_ms" ]] || clipboard_poll_ms="1000"
  latency_report="$(read_config_value "$LATENCY_REPORT_CONFIG_KEY")"; [[ -n "$latency_report" ]] || latency_report="false"

  peer_lines="No peer state has been recorded yet."
  if [[ -f "$STATE_PATH" ]]; then
    peer_lines="$(awk -F'\t' '
      /^peer=/ {
        sub(/^peer=/, "", $1)
        name=$2; approved=$3; connected=$4; last_seen=$5; last_connected=$6
        if (name == "") name="unknown"
        printf "- %s (%s): paired=%s connected=%s last_seen=%s last_connected=%s\n", name, $1, approved, connected, last_seen, last_connected
      }
    ' "$STATE_PATH")"
    [[ -n "$peer_lines" ]] || peer_lines="No peer entries found in $STATE_PATH."
  fi

  quality_text="Service: $(service_state_label "$state") ($state)
Configured host: ${host:-not configured}
Input port: $port
Clipboard port: 15100
Reconnect mode: $( [[ "$auto_connect_enabled" == "true" ]] && printf 'Auto' || printf 'Manual' )
Reconnect timing: initial=${reconnect_initial_backoff_ms}ms max=${reconnect_max_backoff_ms}ms idle=${reconnect_idle_retry_ms}ms
Clipboard: enabled=$clipboard_enabled send_local=$clipboard_send_enabled force_poll=$clipboard_force_poll poll=${clipboard_poll_ms}ms
Latency report logging: $latency_report

Known peers:
$peer_lines"
  zenity --text-info --title="$APP_NAME connection quality" --width=840 --height=520 <<<"$quality_text"
}

diagnostics_bundle() {
  if [[ ! -x "$DIAGNOSTICS_BUNDLE_SCRIPT" ]]; then
    zenity --error --text="Diagnostics bundle script is not available:
$DIAGNOSTICS_BUNDLE_SCRIPT"
    return 1
  fi
  local output_dir result
  output_dir="$(zenity --file-selection --directory --title="$APP_NAME diagnostics output folder" || true)"
  [[ -n "$output_dir" ]] || return 1
  result="$("$DIAGNOSTICS_BUNDLE_SCRIPT" --config "$CONFIG_PATH" --state "$STATE_PATH" --output "$output_dir" 2>&1 || true)"
  zenity --text-info --title="$APP_NAME diagnostics bundle" --width=760 --height=420 <<<"$result"
}

show_peers() {
  local rows=() configured_host selected_peer selected_host selected_port selected_name selected_action
  configured_host="$(read_config_value host)"
  if [[ -f "$STATE_PATH" ]]; then
    local paired_label connected_label configured_label last_seen_label last_connected_label service_running connected_now
    if service_active; then
      service_running="true"
    else
      service_running="false"
    fi
    while IFS= read -r line; do
      [[ "$line" == peer=* ]] || continue
      IFS=$'\t' read -r host name port approved connected_now last_seen last_connected <<<"${line#peer=}"
      if [[ -z "$last_connected" ]]; then
        last_connected="${last_seen:-0}"
        last_seen="${connected_now:-0}"
        connected_now="false"
      fi
      paired_label="$(format_paired_label "$approved")"
      connected_label="$(format_yes_no "$([[ "$service_running" == "true" && "$connected_now" == "true" ]] && printf 'true' || printf 'false')")"
      configured_label="$(format_yes_no "$([[ "$host" == "$configured_host" ]] && printf 'true' || printf 'false')")"
      last_seen_label="$(format_epoch_label "$last_seen")"
      last_connected_label="$(format_epoch_label "$last_connected")"
      rows+=("${host}|${port}|${name:-unknown}" "$host" "${name:-unknown}" "$port" "$paired_label" "$connected_label" "$configured_label" "$last_seen_label" "$last_connected_label")
    done <"$STATE_PATH"
  fi

  if [[ "${#rows[@]}" -eq 0 ]]; then
    zenity --info --text="No peer state has been recorded yet."
    return 0
  fi

  selected_peer="$(zenity --list --title="Known peers" --width=980 --height=420 \
    --text="Select a peer to manage it." \
    --print-column=1 --hide-column=1 \
    --column="Key" --column="Host" --column="Name" --column="Port" --column="Paired" --column="Connected now" --column="Configured host" --column="Last seen" --column="Last connected" \
    "${rows[@]}" || true)"
  [[ -n "$selected_peer" ]] || return 0

  IFS='|' read -r selected_host selected_port selected_name <<<"$selected_peer"
  selected_action="$(zenity --list --radiolist --title="$APP_NAME peer actions" --width=520 --height=220 \
    --text="Peer: ${selected_name:-unknown} ($selected_host:$selected_port)" \
    --column="Use" --column="Action" \
    TRUE "Use as configured Windows host" \
    FALSE "Edit settings for this peer" \
    FALSE "Forget this peer" || true)"
  [[ -n "$selected_action" ]] || return 0

  case "$selected_action" in
    "Use as configured Windows host")
      set_configured_host "$selected_host"
      ;;
    "Edit settings for this peer")
      edit_settings "$selected_host"
      ;;
    "Forget this peer")
      if zenity --question --title="$APP_NAME peer actions" --width=480 \
        --text="Forget peer ${selected_name:-unknown} at $selected_host:$selected_port?\n\nThis removes it from saved peer state only."; then
        remove_peer_state "$selected_host" "$selected_port"
        zenity --info --text="Removed $selected_host:$selected_port from known peers."
      fi
      ;;
  esac
}

discover_peers() {
  require_client_binary || return 1
  local output port selected_ip configured_host service_running
  port="$(read_config_value port)"
  configured_host="$(read_config_value host)"
  [[ -n "$port" ]] || port="15101"
  if service_active; then
    service_running="true"
  else
    service_running="false"
  fi
  output="$("$APP_BIN" discover --state "$STATE_PATH" --port "$port" --timeout-ms 200 --max-hosts 256 2>&1 || true)"
  mapfile -t candidates < <(printf '%s\n' "$output" | awk '
    /^  / {
      ip = $1
      name = "(unknown)"
      verified = "no"
      network = "(default)"
      for (i = 2; i <= NF; i++) {
        if ($i ~ /^name=/) {
          name = substr($i, 6)
        } else if ($i ~ /^verified=/) {
          verified = substr($i, 10)
        } else if ($i ~ /^iface=/) {
          network = substr($i, 7)
        }
      }
      print ip "|" name "|" verified "|" network
    }
  ')
  if [[ "${#candidates[@]}" -eq 0 ]]; then
    zenity --info --title="$APP_NAME discovery" --text="$output"
    return 1
  fi

  local rows=()
  local ip item name verified network paired_label connected_label configured_label last_connected_label state_name state_approved state_connected state_last_seen state_last_connected
  for item in "${candidates[@]}"; do
    IFS='|' read -r ip name verified network <<< "$item"
    state_name=""
    state_approved="false"
    state_connected="false"
    state_last_seen="0"
    state_last_connected="0"
    if IFS=$'\t' read -r state_name state_approved state_connected state_last_seen state_last_connected < <(read_peer_state "$ip" "$port" || true); then
      :
    elif [[ "$name" != "(unknown)" ]] &&
      IFS=$'\t' read -r state_name state_approved state_connected state_last_seen state_last_connected < <(read_peer_state_by_verified_name "$name" "$port" || true); then
      :
    fi
    paired_label="$(format_paired_label "$state_approved")"
    connected_label="$(format_yes_no "$([[ "$service_running" == "true" && "$state_connected" == "true" ]] && printf 'true' || printf 'false')")"
    configured_label="$(format_yes_no "$([[ "$ip" == "$configured_host" ]] && printf 'true' || printf 'false')")"
    last_connected_label="$(format_epoch_label "$state_last_connected")"
    rows+=("$ip" "$name" "$network" "$paired_label" "$connected_label" "$configured_label" "$last_connected_label")
  done

  selected_ip="$(zenity --list --title="Discovered peers" --width=900 --height=320 \
    --text="Choose a Windows peer to make it the configured host for InputFlow." \
    --print-column=1 --column="IP" --column="PC name" --column="Network" --column="Paired" --column="Connected now" --column="Configured host" --column="Last connected" "${rows[@]}" || true)"
  [[ -n "$selected_ip" ]] || return 1
  printf '%s\n' "$selected_ip"
}

discover_and_save_peer() {
  local selected host key key_file secret_id auth_count action
  selected="$(discover_peers || true)"
  if [[ -z "$selected" ]]; then
    return 1
  fi

  # Always prompt for settings to ensure key is entered/verified
  edit_settings "$selected" || return 1

  if ! service_active; then
    if zenity --question --title="$APP_NAME" --width=480 \
      --text="Peer setup is saved.\n\nStart the $APP_NAME background service now?"; then
      start_session
    fi
  fi
}

export_windows_helper() {
  require_client_binary || return 1
  local output_dir position result
  output_dir="$(zenity --file-selection --directory --title="$APP_NAME Windows helper output folder" || true)"
  [[ -n "$output_dir" ]] || return 1
  position="$(zenity --list --radiolist --title="$APP_NAME Windows helper" --width=560 --height=260 \
    --text="Choose where the Linux desktop sits relative to the Windows host." \
    --column="Use" --column="Position" \
    TRUE "auto" \
    FALSE "top-left" \
    FALSE "top-right" \
    FALSE "bottom-left" \
    FALSE "bottom-right" || true)"
  [[ -n "$position" ]] || return 1
  result="$("$APP_BIN" export-windows-pair --config "$CONFIG_PATH" --output "$output_dir" --position "$position" --force 2>&1 || true)"
  zenity --text-info --title="$APP_NAME Windows pairing helper" --width=820 --height=440 <<<"$result

Next steps:
1. Copy the exported .ps1 file to the Windows machine.
2. Start PowerToys once so Mouse Without Borders creates its settings file.
3. Run the helper in PowerShell.
4. Return here and run Health Check."
}

sanitize_topology_name() {
  local value="$1"
  value="$(printf '%s' "$value" | tr -cs 'A-Za-z0-9_.-' '_' | sed 's/^_*//;s/_*$//')"
  [[ -n "$value" ]] || value="machine"
  printf '%s\n' "$value"
}

topology_default_machine_a() {
  local machine_name
  machine_name="$(read_config_value machine_name)"
  [[ -n "$machine_name" ]] || machine_name="$(hostname -s 2>/dev/null || printf 'linux')"
  sanitize_topology_name "$machine_name"
}

topology_default_machine_b() {
  local host
  host="$(read_config_value host)"
  [[ -n "$host" ]] || host="windows"
  sanitize_topology_name "$host"
}

topology_append_display() {
  local id="$1" machine="$2" x="$3" y="$4" width="$5" height="$6"
  printf 'display=%s,%s,%s,%s,%s,%s\n' "$id" "$machine" "$x" "$y" "$width" "$height"
}

topology_append_link() {
  local source="$1" exit_edge="$2" target="$3" entry_edge="$4"
  printf 'link=%s,%s,%s,%s\n' "$source" "$exit_edge" "$target" "$entry_edge"
}

generate_topology_content() {
  local preset="$1" machine_a="$2" machine_b="$3" width="$4" height="$5" wrap_policy="$6" manual_content="${7:-}"
  local a1="${machine_a}-1" a2="${machine_a}-2" b1="${machine_b}-1"
  local x1=0 x2="$width" x3 y2="$height"

  if [[ "$preset" == "manual" ]]; then
    printf '%s\n' "$manual_content"
    return 0
  fi

  x3=$((width * 2))

  cat <<EOF
# InputFlow topology file
# format=inputflow-topology-draft-v1
# preset=$preset
# $TOPOLOGY_FILE_CONFIG_KEY/$TOPOLOGY_ENABLED_CONFIG_KEY enable topology-aware
# runtime handoff. The preview step below only shows the file before writing it.
wrap=$wrap_policy
machine=$machine_a
machine=$machine_b
EOF

  case "$preset" in
    side-by-side)
      topology_append_display "$a1" "$machine_a" "$x1" 0 "$width" "$height"
      topology_append_display "$b1" "$machine_b" "$x2" 0 "$width" "$height"
      topology_append_link "$a1" right "$b1" left
      topology_append_link "$b1" left "$a1" right
      ;;
    stacked)
      topology_append_display "$a1" "$machine_a" 0 0 "$width" "$height"
      topology_append_display "$b1" "$machine_b" 0 "$y2" "$width" "$height"
      topology_append_link "$a1" down "$b1" up
      topology_append_link "$b1" up "$a1" down
      ;;
    AAB)
      topology_append_display "$a1" "$machine_a" "$x1" 0 "$width" "$height"
      topology_append_display "$a2" "$machine_a" "$x2" 0 "$width" "$height"
      topology_append_display "$b1" "$machine_b" "$x3" 0 "$width" "$height"
      topology_append_link "$a1" right "$a2" left
      topology_append_link "$a2" left "$a1" right
      topology_append_link "$a2" right "$b1" left
      topology_append_link "$b1" left "$a2" right
      ;;
    BAA)
      topology_append_display "$b1" "$machine_b" "$x1" 0 "$width" "$height"
      topology_append_display "$a1" "$machine_a" "$x2" 0 "$width" "$height"
      topology_append_display "$a2" "$machine_a" "$x3" 0 "$width" "$height"
      topology_append_link "$b1" right "$a1" left
      topology_append_link "$a1" left "$b1" right
      topology_append_link "$a1" right "$a2" left
      topology_append_link "$a2" left "$a1" right
      ;;
    ABA)
      topology_append_display "$a1" "$machine_a" "$x1" 0 "$width" "$height"
      topology_append_display "$b1" "$machine_b" "$x2" 0 "$width" "$height"
      topology_append_display "$a2" "$machine_a" "$x3" 0 "$width" "$height"
      topology_append_link "$a1" right "$b1" left
      topology_append_link "$b1" left "$a1" right
      topology_append_link "$b1" right "$a2" left
      topology_append_link "$a2" left "$b1" right
      ;;
  esac
}

layout_wizard() {
  local preset preset_label machine_a machine_b display_width display_height wrap_policy file_name
  local fields values gui_output topology_dir topology_path topology_content preview_path manual_template

  preset_label="$(zenity --list --title="$APP_NAME advanced topology/layout wizard" --width=820 --height=430 \
    --text="Topology is optional. If this Fedora/Linux machine has one monitor, use PowerToys layout only and skip topology. Use topology only for multiple Linux displays, wrap, stacked/asymmetric layouts, or wrong-edge handoff problems." \
    --column="Layout" --column="Diagram" --column="Use when" \
    "Use PowerToys layout only" "no topology file" "One Linux/Fedora monitor; normal MWB-style setup" \
    "Linux left, Windows right" "Linux | Windows" "One Linux display beside Windows" \
    "Linux above Windows" "Linux / Windows" "One display stacked above the other" \
    "Two Linux displays, then Windows" "Linux | Linux | Windows" "AAB: dual Linux monitors with Windows on the far right" \
    "Windows, then two Linux displays" "Windows | Linux | Linux" "BAA: Windows on the far left" \
    "Linux split around Windows" "Linux | Windows | Linux" "ABA: Windows between two Linux displays" \
    "Advanced/manual topology" "custom" "Asymmetric, unusual, or hand-edited layouts" || true)"
  [[ -n "$preset_label" ]] || return 1

  case "$preset_label" in
    "Use PowerToys layout only") disable_topology; return $? ;;
    "Linux left, Windows right") preset="side-by-side" ;;
    "Linux above Windows") preset="stacked" ;;
    "Two Linux displays, then Windows") preset="AAB" ;;
    "Windows, then two Linux displays") preset="BAA" ;;
    "Linux split around Windows") preset="ABA" ;;
    "Advanced/manual topology") preset="manual" ;;
    *) preset="$preset_label" ;;
  esac

  machine_a="$(topology_default_machine_a)"
  machine_b="$(topology_default_machine_b)"
  display_width="$(read_config_value screen_width)"; [[ -n "$display_width" ]] || display_width="1920"
  display_height="$(read_config_value screen_height)"; [[ -n "$display_height" ]] || display_height="1080"
  wrap_policy="none"
  file_name="topology-${preset}.topology"

  if [[ "$preset" == "manual" ]]; then
    manual_template="$(generate_topology_content "side-by-side" "$machine_a" "$machine_b" "$display_width" "$display_height" "$wrap_policy")"
    preview_path="$(mktemp)"
    printf '%s\n' "$manual_template" >"$preview_path"
    topology_content="$(zenity --text-info --editable --title="$APP_NAME manual topology" --width=760 --height=520 \
      --filename="$preview_path" || true)"
    rm -f "$preview_path"
    [[ -n "$topology_content" ]] || return 1
  else
    fields="machine_a:Linux Machine Name:entry||machine_b:Windows Machine Name:entry||display_width:Display Width:entry||display_height:Display Height:entry||wrap_policy:Wrap Policy|none|horizontal|vertical|both:combo||file_name:Topology File Name:entry"
    values="$machine_a|$machine_b|$display_width|$display_height|$wrap_policy|$file_name"
    gui_output="$(python3 "$SCRIPT_DIR/src/ConfigDialog.py" "$APP_NAME topology/layout wizard" "$fields" "$values" || true)"
    [[ -n "$gui_output" ]] || return 1
    IFS='|' read -r machine_a machine_b display_width display_height wrap_policy file_name <<< "$gui_output"

    machine_a="$(sanitize_topology_name "$machine_a")"
    machine_b="$(sanitize_topology_name "$machine_b")"
    if ! is_integer_in_range "$display_width" 1 100000; then zenity --error --text="Display width must be a positive integer."; return 1; fi
    if ! is_integer_in_range "$display_height" 1 100000; then zenity --error --text="Display height must be a positive integer."; return 1; fi
    topology_content="$(generate_topology_content "$preset" "$machine_a" "$machine_b" "$display_width" "$display_height" "$wrap_policy")"
  fi

  file_name="$(basename "${file_name:-topology-${preset}.topology}")"
  [[ "$file_name" == *.topology ]] || file_name="${file_name}.topology"
  topology_dir="$(dirname "$CONFIG_PATH")"
  topology_path="$topology_dir/$file_name"

  preview_path="$(mktemp)"
  printf '%s\n' "$topology_content" >"$preview_path"
  if ! zenity --text-info --title="$APP_NAME topology dry-run preview" --width=820 --height=560 \
      --filename="$preview_path" --ok-label="Continue" --cancel-label="Back"; then
    rm -f "$preview_path"
    return 1
  fi
  rm -f "$preview_path"

  if ! zenity --question --title="$APP_NAME advanced topology/layout wizard" --width=620 \
      --text="Apply this advanced topology?\n\nFor one Linux monitor, cancel and use PowerToys layout only.\n\nWill write:\n$topology_path\n\nWill set:\n$TOPOLOGY_ENABLED_CONFIG_KEY=true\n$TOPOLOGY_FILE_CONFIG_KEY=$topology_path\n\nTopology will enforce configured cross-machine edge handoffs at runtime. Same-machine edges remain local."; then
    return 1
  fi

  mkdir -p "$topology_dir"
  printf '%s\n' "$topology_content" >"$topology_path"
  write_topology_config_keys "$topology_path"
  zenity --info --width=680 --text="Topology saved.\n\nFile: $topology_path\nConfig: $CONFIG_PATH\n\nWindows PowerToys still owns the Windows-side machine layout. Keep the PowerToys Linux/Windows machine position consistent with this topology."
  offer_service_restart_if_active "Topology settings updated."
}

disable_topology() {
  if ! zenity --question --title="$APP_NAME topology" --width=620 \
      --text="Use PowerToys layout only?\n\nThis disables InputFlow topology by setting:\n$TOPOLOGY_ENABLED_CONFIG_KEY=false\n\nThis is the recommended mode for a single Fedora/Linux monitor. PowerToys continues to decide the Linux/Windows machine placement."; then
    return 1
  fi

  disable_topology_config
  zenity --info --width=620 --text="Topology disabled.\n\nInputFlow will use the normal PowerToys/MWB-style machine layout path. No topology file is required for a single Linux monitor."
  offer_service_restart_if_active "Topology disabled."
}

explain_topology() {
  require_client_binary || return 1
  local explanation
  explanation="$("$APP_BIN" topology explain --config "$CONFIG_PATH" 2>&1 || true)"
  zenity --text-info --title="$APP_NAME topology explanation" --width=860 --height=620 <<<"$explanation"
}

guided_pairing() {
  while true; do
    local choice
    choice="$(zenity --list --title="$APP_NAME guided pairing" --width=620 --height=390 \
      --text="Use this flow to discover Windows, save Linux settings, export the Windows helper, then verify the setup. Topology is optional; skip it for one Linux monitor." \
      --column="Step" \
      "1. Discover Windows peer and save settings" \
      "2. Edit settings manually" \
      "3. Export Windows helper" \
      "4. Start service" \
      "5. Run health check" \
      "Optional: Advanced topology/layout" \
      "Optional: Use PowerToys layout only" \
      "Optional: Explain current topology" \
      "Back" || true)"
    case "$choice" in
      "1. Discover Windows peer and save settings") discover_and_save_peer ;;
      "2. Edit settings manually") edit_settings ;;
      "3. Export Windows helper") export_windows_helper ;;
      "4. Start service") start_session ;;
      "5. Run health check") health_check ;;
      "Optional: Advanced topology/layout") layout_wizard ;;
      "Optional: Use PowerToys layout only") disable_topology ;;
      "Optional: Explain current topology") explain_topology ;;
      ""|"Back") return 0 ;;
    esac
  done
}

edit_settings() {
  local preset_host="${1:-}"
  local host key key_file secret_id secret_key_name machine_name port screen_width screen_height auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms clipboard_enabled clipboard_force_poll clipboard_poll_ms
  local clipboard_send_enabled current_auth_mode auth_action key_mode cleanup_secret_id saved_message
  local mpris_media_keys_enabled mpris_player latency_report gui_output

  host="$(read_config_value host)"
  key="$(read_config_value key)"
  key_file="$(read_config_value key_file)"
  secret_id="$(read_secret_id_value)"
  secret_key_name="$(detect_secret_id_key_name)"
  machine_name="$(read_config_value machine_name)"
  port="$(read_config_value port)"; [[ -n "$port" ]] || port="15101"
  screen_width="$(read_config_value screen_width)"
  screen_height="$(read_config_value screen_height)"
  IFS=$'\t' read -r auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms < <(read_connection_behavior_values)
  clipboard_enabled="$(read_config_value clipboard_enabled)"; [[ -n "$clipboard_enabled" ]] || clipboard_enabled="true"
  clipboard_send_enabled="$(read_config_value clipboard_send_enabled)"; [[ -n "$clipboard_send_enabled" ]] || clipboard_send_enabled="true"
  clipboard_force_poll="$(read_config_value clipboard_force_poll)"; [[ -n "$clipboard_force_poll" ]] || clipboard_force_poll="false"
  clipboard_poll_ms="$(read_config_value clipboard_poll_ms)"; [[ -n "$clipboard_poll_ms" ]] || clipboard_poll_ms="1000"
  mpris_media_keys_enabled="$(read_config_value "$MPRIS_MEDIA_KEYS_CONFIG_KEY")"; [[ -n "$mpris_media_keys_enabled" ]] || mpris_media_keys_enabled="true"
  mpris_player="$(read_config_value "$MPRIS_PLAYER_CONFIG_KEY")"
  latency_report="$(read_config_value "$LATENCY_REPORT_CONFIG_KEY")"; [[ -n "$latency_report" ]] || latency_report="false"

  current_auth_mode="$(configured_auth_mode "$key" "$key_file" "$secret_id")"

  local fields="host:Windows Host:entry||machine_name:Local Machine Name:entry||port:Network Port:entry||screen_width:Screen Width:entry||screen_height:Screen Height:entry||clipboard_poll_ms:Clipboard Poll (ms):entry||mpris_player:MPRIS Player:entry||clipboard_enabled:Sync Clipboard:switch||clipboard_send_enabled:Send Local Clipboard:switch||clipboard_force_poll:Force Wayland Polling:switch||mpris_media_keys_enabled:Enable Media Keys:switch||latency_report:Print Latency Report:switch"
  local values="${preset_host:-$host}|$machine_name|$port|$screen_width|$screen_height|$clipboard_poll_ms|$mpris_player|$clipboard_enabled|$clipboard_send_enabled|$clipboard_force_poll|$mpris_media_keys_enabled|$latency_report"

  gui_output="$(python3 "$SCRIPT_DIR/src/ConfigDialog.py" "$APP_NAME Settings" "$fields" "$values" || true)"
  [[ -n "$gui_output" ]] || return 1

  IFS='|' read -r host machine_name port screen_width screen_height clipboard_poll_ms mpris_player clipboard_enabled clipboard_send_enabled clipboard_force_poll mpris_media_keys_enabled latency_report <<< "$gui_output"

  # Validation
  if ! is_integer_in_range "$port" 1 65535; then zenity --error --text="Port must be 1-65535."; return 1; fi

  # Authentication (Keep Zenity for secret-tool branching)
  while true; do
    auth_action="$(zenity --list --radiolist --title="$APP_NAME Auth" --width=500 --height=220 \
      --text="Method: $current_auth_mode" \
      --column="Use" --column="Action" \
      TRUE "Change method" \
      FALSE "Reveal key" \
      FALSE "Continue" || true)"
    [[ -n "$auth_action" ]] || return 1
    [[ "$auth_action" == "Continue" ]] && break
    if [[ "$auth_action" == "Reveal key" ]]; then
      show_current_security_key "$key" "$key_file" "$secret_id" "$current_auth_mode" || true
      continue
    fi

    key_mode="$(zenity --list --radiolist --title="$APP_NAME Method" --width=500 --height=220 \
      --column="Use" --column="Method" \
      $([[ "$current_auth_mode" == "Inline security key" ]] && printf 'TRUE' || printf 'FALSE') "Inline security key" \
      $([[ "$current_auth_mode" == "Key file path" ]] && printf 'TRUE' || printf 'FALSE') "Key file path" \
      $([[ "$current_auth_mode" == "Secret Service entry" ]] && printf 'TRUE' || printf 'FALSE') "Secret Service entry" || true)"
    [[ -n "$key_mode" ]] || return 1

    if [[ "$key_mode" == "Inline security key" ]]; then
      local entered_key
      entered_key="$(zenity --password --title="$APP_NAME Key" --text="Security key" || true)"
      if [[ -n "$entered_key" ]]; then key="$entered_key"; fi
      key_file=""; cleanup_secret_id="$(choose_secret_cleanup_target "$secret_id" "$key_mode" "" || true)"; secret_id=""
    elif [[ "$key_mode" == "Key file path" ]]; then
      local entered_key_file
      entered_key_file="$(zenity --file-selection --title="$APP_NAME Key File" || true)"
      [[ -n "$entered_key_file" ]] || return 1
      key_file="$entered_key_file"; key=""; cleanup_secret_id="$(choose_secret_cleanup_target "$secret_id" "$key_mode" "" || true)"; secret_id=""
    else
      local entered_secret_id entered_secret_key
      entered_secret_id="$(zenity --entry --title="$APP_NAME Secret ID" --text="Identifier" --entry-text="$secret_id" || true)"
      [[ -n "$entered_secret_id" ]] || return 1
      entered_secret_key="$(zenity --password --title="$APP_NAME Secret Key" --text="Key to store" || true)"
      if [[ -n "$entered_secret_key" ]]; then store_secret_service_key "$entered_secret_id" "$entered_secret_key" || return 1; fi
      key_file=""; key=""; cleanup_secret_id="$(choose_secret_cleanup_target "$secret_id" "$key_mode" "$entered_secret_id" || true)"; secret_id="$entered_secret_id"
    fi
    current_auth_mode="$key_mode"
    break
  done

  write_config "$host" "$key" "$key_file" "$secret_id" "$machine_name" "$port" "$auto_connect_enabled" "$reconnect_initial_backoff_ms" "$reconnect_max_backoff_ms" "$reconnect_idle_retry_ms" "$clipboard_enabled" "$clipboard_send_enabled" "$clipboard_force_poll" "$clipboard_poll_ms" "$screen_width" "$screen_height" "$mpris_media_keys_enabled" "$mpris_player" "$latency_report" "$secret_key_name"
  zenity --info --text="Settings saved."
  offer_service_restart_if_active "Settings updated."
}

edit_connection_behavior() {
  local host key key_file secret_id secret_key_name machine_name port screen_width screen_height auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms clipboard_enabled clipboard_send_enabled clipboard_force_poll clipboard_poll_ms mpris_media_keys_enabled mpris_player latency_report
  local gui_output

  host="$(read_config_value host)"
  key="$(read_config_value key)"
  key_file="$(read_config_value key_file)"
  secret_id="$(read_secret_id_value)"
  secret_key_name="$(detect_secret_id_key_name)"
  machine_name="$(read_config_value machine_name)"
  port="$(read_config_value port)"; [[ -n "$port" ]] || port="15101"
  screen_width="$(read_config_value screen_width)"
  screen_height="$(read_config_value screen_height)"
  IFS=$'\t' read -r auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms < <(read_connection_behavior_values)
  clipboard_enabled="$(read_config_value clipboard_enabled)"; [[ -n "$clipboard_enabled" ]] || clipboard_enabled="true"
  clipboard_send_enabled="$(read_config_value clipboard_send_enabled)"; [[ -n "$clipboard_send_enabled" ]] || clipboard_send_enabled="true"
  clipboard_force_poll="$(read_config_value clipboard_force_poll)"; [[ -n "$clipboard_force_poll" ]] || clipboard_force_poll="false"
  clipboard_poll_ms="$(read_config_value clipboard_poll_ms)"; [[ -n "$clipboard_poll_ms" ]] || clipboard_poll_ms="1000"
  mpris_media_keys_enabled="$(read_config_value "$MPRIS_MEDIA_KEYS_CONFIG_KEY")"; [[ -n "$mpris_media_keys_enabled" ]] || mpris_media_keys_enabled="true"
  mpris_player="$(read_config_value "$MPRIS_PLAYER_CONFIG_KEY")"
  latency_report="$(read_config_value "$LATENCY_REPORT_CONFIG_KEY")"; [[ -n "$latency_report" ]] || latency_report="false"

  local fields="mode:Connection Mode|Auto-reconnect to host|Manual start only:combo||initial:Initial Retry Delay (ms):entry||max:Maximum Retry Delay (ms):entry||idle:Idle Retry Interval (ms):entry"
  local mode_val="$( [[ "$auto_connect_enabled" == "true" ]] && printf 'Auto-reconnect to host' || printf 'Manual start only' )"
  local values="$mode_val|$reconnect_initial_backoff_ms|$reconnect_max_backoff_ms|$reconnect_idle_retry_ms"

  gui_output="$(python3 "$SCRIPT_DIR/src/ConfigDialog.py" "$APP_NAME Connection" "$fields" "$values" || true)"
  [[ -n "$gui_output" ]] || return 1

  IFS='|' read -r mode_label reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms <<< "$gui_output"

  auto_connect_enabled="false"
  [[ "$mode_label" == "Auto-reconnect to host" ]] && auto_connect_enabled="true"

  write_config "$host" "$key" "$key_file" "$secret_id" "$machine_name" "$port" "$auto_connect_enabled" "$reconnect_initial_backoff_ms" "$reconnect_max_backoff_ms" "$reconnect_idle_retry_ms" "$clipboard_enabled" "$clipboard_send_enabled" "$clipboard_force_poll" "$clipboard_poll_ms" "$screen_width" "$screen_height" "$mpris_media_keys_enabled" "$mpris_player" "$latency_report" "$secret_key_name"
  zenity --info --text="Connection behavior saved."
  offer_service_restart_if_active "Connection behavior updated."
}

show_tray_visibility_help() {
  local desktop_name="this desktop environment"
  local desktop_env="${XDG_CURRENT_DESKTOP:-${DESKTOP_SESSION:-}}"

  if [[ -n "$desktop_env" ]]; then
    desktop_name="$desktop_env"
  fi

  zenity --info --title="$APP_NAME tray help" --width=560 \
    --text="The tray controller may be hidden even when it is running.\n\nDesktop: $desktop_name\n\nTips:\n• On KDE Plasma, open the system tray overflow and set InputFlow to Always Shown.\n• The tray may use a short IF label plus the teal InputFlow icon.\n• You can always open the controller directly from the app launcher or by running:\n  $SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")\n• If the tray still does not appear, install desktop entries from the controller and launch InputFlow Controller from the application menu."
}

start_session() {
  require_client_binary || return 1
  if [[ ! -f "$CONFIG_PATH" ]]; then
    "$APP_BIN" init-config --config "$CONFIG_PATH" --force >/dev/null
  fi

  local host key key_file secret_id auth_count resolved_key_file
  host="$(read_config_value host)"
  key="$(read_config_value key)"
  key_file="$(read_config_value key_file)"
  secret_id="$(read_secret_id_value)"
  auth_count="$(configured_auth_source_count "$key" "$key_file" "$secret_id")"

  if [[ -z "$host" ]]; then
    zenity --error --text="Set a Windows host before starting."
    return 1
  fi

  if (( auth_count == 0 )); then
    zenity --error --text="Set exactly one authentication method before starting: inline key, key file, or Secret Service entry."
    return 1
  fi

  if (( auth_count > 1 )); then
    zenity --error --text="Multiple authentication methods are configured. Keep only one of: inline key, key file, or Secret Service entry."
    return 1
  fi

  if [[ -n "$key_file" ]]; then
    resolved_key_file="$(resolve_config_relative_path "$key_file")"
    if [[ ! -r "$resolved_key_file" ]]; then
      zenity --error --text="Key file is not readable: $resolved_key_file"
      return 1
    fi
  fi

  if [[ -n "$secret_id" && -z "$(trim_whitespace "$secret_id")" ]]; then
    zenity --error --text="Secret Service authentication requires a non-empty identifier."
    return 1
  fi

  install_service
  systemctl --user enable --now "$SERVICE_NAME" >/dev/null
  systemctl --user restart "$SERVICE_NAME" >/dev/null
  zenity --info --text="Started the $APP_NAME background service."
}

stop_session() {
  systemctl --user stop "$SERVICE_NAME" >/dev/null 2>&1 || true
  zenity --info --text="Stopped the $APP_NAME background service."
}

restart_session() {
  install_service
  systemctl --user restart "$SERVICE_NAME" >/dev/null 2>&1 || true
  zenity --info --text="Restarted the $APP_NAME background service."
}

install_desktop_entry() {
  local entry_dir="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
  local entry_path="$entry_dir/mwb-client-ui.desktop"
  local tray_entry_path="$entry_dir/mwb-client-tray.desktop"
  local desktop_icon="network-workgroup"
  if [[ -n "${APP_ICON_PATH:-}" ]]; then
    desktop_icon="$APP_ICON_PATH"
  fi
  mkdir -p "$entry_dir"
  cat >"$entry_path" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=InputFlow Controller
Comment=Open settings and service controls for InputFlow
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")
Icon=$desktop_icon
Terminal=false
Categories=Utility;Network;
Keywords=mouse;keyboard;sharing;input;controller;
StartupNotify=false
Actions=GuidedPairing;DisableTopology;TopologyWizard;ExplainTopology;HealthCheck;DiagnosticsBundle;ConnectionQuality;OpenSettings;OpenConnectionBehavior;ShowTrayHelp;ShowStatus;StartService;RestartService;StopService;

[Desktop Action GuidedPairing]
Name=Guided Pairing
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") guided-pairing

[Desktop Action HealthCheck]
Name=Health Check
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") health-check

[Desktop Action DisableTopology]
Name=Use PowerToys Layout Only
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") disable-topology

[Desktop Action TopologyWizard]
Name=Advanced Topology/Layout
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") layout-wizard

[Desktop Action ExplainTopology]
Name=Explain Current Topology
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") explain-topology

[Desktop Action DiagnosticsBundle]
Name=Diagnostics Bundle
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") diagnostics-bundle

[Desktop Action ConnectionQuality]
Name=Connection Quality
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") connection-quality

[Desktop Action OpenSettings]
Name=Open Settings
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") settings

[Desktop Action OpenConnectionBehavior]
Name=Connection Behavior
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") connection

[Desktop Action ShowTrayHelp]
Name=Tray Visibility Help
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") tray-help

[Desktop Action ShowStatus]
Name=Show Service Status
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") status

[Desktop Action StartService]
Name=Start Service
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") start

[Desktop Action RestartService]
Name=Restart Service
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") restart

[Desktop Action StopService]
Name=Stop Service
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") stop
EOF

  if [[ -n "${TRAY_BIN:-}" ]]; then
    cat >"$tray_entry_path" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=InputFlow Tray
Comment=Background tray controller for InputFlow
Exec=$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}") tray
Icon=$desktop_icon
Terminal=false
Categories=Utility;Network;
Keywords=mouse;keyboard;sharing;input;tray;
StartupNotify=false
EOF
    zenity --info --text="Installed desktop entries:\n$entry_path\n$tray_entry_path"
    return 0
  fi

  zenity --info --text="Installed desktop entry to $entry_path"
}

main_menu() {
  while true; do
    local choice
    choice="$(zenity --list --title="$APP_NAME" --text="$(menu_summary_text)" --width=540 --height=430 \
      --column="Action" \
      "Guided Pairing" \
      "Use PowerToys Layout Only" \
      "Advanced Topology/Layout" \
      "Explain Current Topology" \
      "Health Check" \
      "Diagnostics Bundle" \
      "Connection Quality" \
      "Settings" \
      "Peers (Discovery & Known)" \
      "Connection Behavior" \
      "Start Service" \
      "Stop Service" \
      "Restart Service" \
      "Show Service Details" \
      "Install Desktop Entries" \
      "Tray Visibility Help" \
      "Quit" || true)"

    case "$choice" in
      "Guided Pairing") guided_pairing ;;
      "Use PowerToys Layout Only") disable_topology ;;
      "Advanced Topology/Layout") layout_wizard ;;
      "Explain Current Topology") explain_topology ;;
      "Health Check") health_check ;;
      "Diagnostics Bundle") diagnostics_bundle ;;
      "Connection Quality") connection_quality ;;
      "Settings") edit_settings ;;
      "Peers (Discovery & Known)")
        local peer_choice
        peer_choice="$(zenity --list --title="$APP_NAME Peers" --text="Manage peers" --width=400 --height=250 \
          --column="Action" "Discover Peers" "Known Peers" "Back" || true)"
        [[ "$peer_choice" == "Discover Peers" ]] && discover_and_save_peer
        [[ "$peer_choice" == "Known Peers" ]] && show_peers
        ;;
      "Connection Behavior") edit_connection_behavior ;;
      "Start Service") start_session ;;
      "Stop Service") stop_session ;;
      "Restart Service") restart_session ;;
      "Show Service Details") show_status ;;
      "Install Desktop Entries") install_desktop_entry ;;
      "Tray Visibility Help") show_tray_visibility_help ;;
      ""|"Quit") exit 0 ;;
    esac
  done
}

require_ui

case "${1:-menu}" in
  ""|menu) main_menu ;;
  guided-pairing|pairing|export-helper) guided_pairing ;;
  disable-topology|powertoys-layout-only|simple-layout) disable_topology ;;
  layout-wizard|topology-wizard|topology|layout) layout_wizard ;;
  explain-topology|topology-explain) explain_topology ;;
  health-check|doctor) health_check ;;
  diagnostics-bundle|diagnostics) diagnostics_bundle ;;
  connection-quality|quality) connection_quality ;;
  settings) edit_settings ;;
  connection|connection-behavior|reconnect) edit_connection_behavior ;;
  discover) discover_and_save_peer ;;
  peers) show_peers ;;
  tray-help|visibility-help) show_tray_visibility_help ;;
  status) show_status ;;
  start) start_session ;;
  restart) restart_session ;;
  stop) stop_session ;;
  tray) start_tray ;;
  install-desktop-entry|install-desktop-entries) install_desktop_entry ;;
  help|-h|--help)
    printf 'Usage: %s [menu|guided-pairing|disable-topology|layout-wizard|explain-topology|health-check|diagnostics-bundle|connection-quality|settings|connection|discover|peers|tray-help|status|start|restart|stop|tray|install-desktop-entry]\n' "$(basename "${BASH_SOURCE[0]}")"
    ;;
  *)
    zenity --error --text="Unknown action: $1"
    exit 1
    ;;
esac
