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
  local state host machine_name port key key_file secret_id auth_label auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms
  state="$(service_state)"
  host="$(read_config_value host)"
  machine_name="$(read_config_value machine_name)"
  port="$(read_config_value port)"
  key="$(read_config_value key)"
  key_file="$(read_config_value key_file)"
  secret_id="$(read_secret_id_value)"
  IFS=$'\t' read -r auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms < <(read_connection_behavior_values)
  auth_label="$(configured_auth_label "$key" "$key_file" "$secret_id")"

  [[ -n "$host" ]] || host="not configured"
  [[ -n "$machine_name" ]] || machine_name="not set"
  [[ -n "$port" ]] || port="15101"

  printf 'Service: %s\nConfigured host: %s\nAuthentication: %s\nMachine name: %s\nPort: %s\nConnection: %s (%s-%s ms, idle %s ms)' \
    "$(service_state_label "$state")" \
    "$host" \
    "$auth_label" \
    "$machine_name" \
    "$port" \
    "$(connection_behavior_mode_label "$auto_connect_enabled")" \
    "$reconnect_initial_backoff_ms" \
    "$reconnect_max_backoff_ms" \
    "$reconnect_idle_retry_ms"
}

show_status() {
  local status_text doctor_text
  status_text="$(systemctl --user status --no-pager "$SERVICE_NAME" 2>&1 || true)"
  doctor_text="$("$APP_BIN" doctor --config "$CONFIG_PATH" 2>&1 || true)"
  zenity --text-info --title="$APP_NAME service status" --width=900 --height=600 <<<"$doctor_text

----

$status_text"
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
    FALSE "Forget this peer" || true)"
  [[ -n "$selected_action" ]] || return 0

  case "$selected_action" in
    "Use as configured Windows host")
      set_configured_host "$selected_host"
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
      network = "(default)"
      for (i = 2; i <= NF; i++) {
        if ($i ~ /^name=/) {
          name = substr($i, 6)
        } else if ($i ~ /^iface=/) {
          network = substr($i, 7)
        }
      }
      print ip "|" name "|" network
    }
  ')
  if [[ "${#candidates[@]}" -eq 0 ]]; then
    zenity --info --title="$APP_NAME discovery" --text="$output"
    return 1
  fi

  local rows=()
  local ip item name network paired_label connected_label configured_label last_connected_label state_name state_approved state_connected state_last_seen state_last_connected
  for item in "${candidates[@]}"; do
    IFS='|' read -r ip name network <<< "$item"
    state_name=""
    state_approved="false"
    state_connected="false"
    state_last_seen="0"
    state_last_connected="0"
    if IFS=$'\t' read -r state_name state_approved state_connected state_last_seen state_last_connected < <(read_peer_state "$ip" "$port" || true); then
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

  host="$(read_config_value host)"
  key="$(read_config_value key)"
  key_file="$(read_config_value key_file)"
  secret_id="$(read_secret_id_value)"
  auth_count="$(configured_auth_source_count "$key" "$key_file" "$secret_id")"

  if (( auth_count == 1 )); then
    action="$(zenity --list --radiolist --title="$APP_NAME discovered peer" --width=560 --height=260 \
      --text="Discovered Windows host: $selected\n\nChoose how to use it." \
      --column="Use" --column="Action" \
      TRUE "Use as configured Windows host now" \
      FALSE "Open full settings for this peer" || true)"
    [[ -n "$action" ]] || return 1
    if [[ "$action" == "Use as configured Windows host now" ]]; then
      set_configured_host "$selected"
    else
      edit_settings "$selected" || return 1
    fi
  else
    edit_settings "$selected" || return 1
  fi

  if ! service_active; then
    if zenity --question --title="$APP_NAME" --width=480 \
      --text="Peer setup is saved.\n\nStart the $APP_NAME background service now?"; then
      start_session
    fi
  fi
}

edit_settings() {
  local preset_host="${1:-}"
  local host key key_file secret_id secret_key_name machine_name port screen_width screen_height auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms clipboard_enabled clipboard_force_poll clipboard_poll_ms
  local clipboard_send_enabled current_auth_mode auth_action key_mode cleanup_secret_id saved_message host_dialog_title host_dialog_text host_entry_text
  local mpris_media_keys_enabled mpris_player latency_report
  host="$(read_config_value host)"
  key="$(read_config_value key)"
  key_file="$(read_config_value key_file)"
  secret_id="$(read_secret_id_value)"
  secret_key_name="$(detect_secret_id_key_name)"
  machine_name="$(read_config_value machine_name)"
  port="$(read_config_value port)"
  screen_width="$(read_config_value screen_width)"
  screen_height="$(read_config_value screen_height)"
  IFS=$'\t' read -r auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms < <(read_connection_behavior_values)
  clipboard_enabled="$(read_config_value clipboard_enabled)"
  clipboard_send_enabled="$(read_config_value clipboard_send_enabled)"
  clipboard_force_poll="$(read_config_value clipboard_force_poll)"
  clipboard_poll_ms="$(read_config_value clipboard_poll_ms)"
  mpris_media_keys_enabled="$(read_config_value "$MPRIS_MEDIA_KEYS_CONFIG_KEY")"
  mpris_player="$(read_config_value "$MPRIS_PLAYER_CONFIG_KEY")"
  latency_report="$(read_config_value "$LATENCY_REPORT_CONFIG_KEY")"

  [[ -n "$port" ]] || port="15101"
  [[ -n "$clipboard_enabled" ]] || clipboard_enabled="true"
  [[ -n "$clipboard_send_enabled" ]] || clipboard_send_enabled="true"
  [[ -n "$clipboard_force_poll" ]] || clipboard_force_poll="false"
  [[ -n "$clipboard_poll_ms" ]] || clipboard_poll_ms="1000"
  [[ -n "$mpris_media_keys_enabled" ]] || mpris_media_keys_enabled="true"
  [[ -n "$latency_report" ]] || latency_report="false"
  current_auth_mode="$(configured_auth_mode "$key" "$key_file" "$secret_id")"

  if (( $(configured_auth_source_count "$key" "$key_file" "$secret_id") > 1 )); then
    zenity --warning --text="Multiple authentication sources are configured. Saving settings will keep only the method selected in the next step."
  fi

  if [[ -n "$preset_host" ]]; then
    host_dialog_title="$APP_NAME add discovered peer"
    if [[ -n "$host" && "$host" != "$preset_host" ]]; then
      host_dialog_text="Discovered Windows host. Saving this replaces the currently configured host.\n\nCurrent host: $host\nDiscovered host: $preset_host"
    else
      host_dialog_text="Discovered Windows host. Saving this sets the configured host used by InputFlow."
    fi
    host_entry_text="$preset_host"
  else
    host_dialog_title="$APP_NAME settings"
    host_dialog_text="Configured Windows host.\n\nEdit the current host entry used by InputFlow."
    host_entry_text="$host"
  fi

  host="$(zenity --entry --title="$host_dialog_title" --text="$host_dialog_text" --entry-text="$host_entry_text" || true)"
  [[ -n "$host" ]] || return 1

  while true; do
    auth_action="$(zenity --list --radiolist --title="$APP_NAME authentication" --width=520 --height=220 \
      --text="Current authentication: $current_auth_mode" \
      --column="Use" --column="Action" \
      TRUE "Edit authentication settings" \
      FALSE "Reveal current key" || true)"
    [[ -n "$auth_action" ]] || return 1
    if [[ "$auth_action" != "Reveal current key" ]]; then
      break
    fi
    show_current_security_key "$key" "$key_file" "$secret_id" "$current_auth_mode" || true
  done

  key_mode="$(zenity --list --radiolist --title="$APP_NAME authentication" --width=500 --height=220 \
    --column="Use" --column="Method" \
    $([[ "$current_auth_mode" == "Inline security key" ]] && printf 'TRUE' || printf 'FALSE') "Inline security key" \
    $([[ "$current_auth_mode" == "Key file path" ]] && printf 'TRUE' || printf 'FALSE') "Key file path" \
    $([[ "$current_auth_mode" == "Secret Service entry" ]] && printf 'TRUE' || printf 'FALSE') "Secret Service entry" || true)"
  [[ -n "$key_mode" ]] || return 1

  if [[ "$key_mode" == "Inline security key" ]]; then
    local entered_key
    entered_key="$(zenity --password --title="$APP_NAME settings" --text="Security key$([[ "$current_auth_mode" == "Inline security key" && -n "$key" ]] && printf ' (leave blank to keep current key)')" || true)"
    if [[ -n "$entered_key" ]]; then
      key="$entered_key"
    elif [[ -z "$key" || "$current_auth_mode" != "Inline security key" ]]; then
      zenity --error --text="Enter a security key to use inline."
      return 1
    fi
    key_file=""
    cleanup_secret_id="$(choose_secret_cleanup_target "$secret_id" "$key_mode" "" || true)"
    secret_id=""
  else
    key=""
    if [[ "$key_mode" == "Key file path" ]]; then
      local entered_key_file key_file_dialog_path
      key_file_dialog_path="${key_file:-$HOME/}"
      if [[ -n "$key_file" ]]; then
        key_file_dialog_path="$(resolve_config_relative_path "$key_file")"
      fi
      entered_key_file="$(zenity --file-selection --title="$APP_NAME settings" --filename="$key_file_dialog_path" || true)"
      [[ -n "$entered_key_file" ]] || return 1
      key_file="$entered_key_file"
      cleanup_secret_id="$(choose_secret_cleanup_target "$secret_id" "$key_mode" "" || true)"
      secret_id=""
    else
      local entered_secret_id entered_secret_key
      entered_secret_id="$(zenity --entry --title="$APP_NAME settings" --text="Secret Service identifier" --entry-text="$secret_id" || true)"
      [[ -n "$entered_secret_id" ]] || return 1
      entered_secret_key="$(zenity --password --title="$APP_NAME settings" --text="Security key to store in Secret Service$([[ "$current_auth_mode" == "Secret Service entry" && "$entered_secret_id" == "$secret_id" ]] && printf ' (leave blank to keep the stored key)')" || true)"
      if [[ -n "$entered_secret_key" ]]; then
        store_secret_service_key "$entered_secret_id" "$entered_secret_key" || return 1
      elif [[ "$current_auth_mode" != "Secret Service entry" || "$entered_secret_id" != "$secret_id" ]]; then
        zenity --error --text="Enter a security key to store for the selected Secret Service identifier."
        return 1
      fi
      unset entered_secret_key
      key_file=""
      cleanup_secret_id="$(choose_secret_cleanup_target "$secret_id" "$key_mode" "$entered_secret_id" || true)"
      secret_id="$entered_secret_id"
    fi
  fi

  machine_name="$(zenity --entry --title="$APP_NAME settings" --text="Machine name" --entry-text="$machine_name" || true)"
  [[ -n "$machine_name" ]] || machine_name=""
  port="$(zenity --entry --title="$APP_NAME settings" --text="Port" --entry-text="$port" || true)"
  [[ -n "$port" ]] || return 1
  if ! is_integer_in_range "$port" 1 65535; then
    zenity --error --text="Port must be an integer between 1 and 65535."
    return 1
  fi
  screen_width="$(zenity --entry --title="$APP_NAME settings" --text="Screen width override (blank for automatic)" --entry-text="$screen_width" || true)"
  if [[ -n "$screen_width" ]] && ! is_integer_in_range "$screen_width" 1 2147483647; then
    zenity --error --text="Screen width must be blank or a positive integer."
    return 1
  fi
  screen_height="$(zenity --entry --title="$APP_NAME settings" --text="Screen height override (blank for automatic)" --entry-text="$screen_height" || true)"
  if [[ -n "$screen_height" ]] && ! is_integer_in_range "$screen_height" 1 2147483647; then
    zenity --error --text="Screen height must be blank or a positive integer."
    return 1
  fi
  if { [[ -n "$screen_width" && -z "$screen_height" ]] || [[ -z "$screen_width" && -n "$screen_height" ]]; }; then
    zenity --error --text="Set both screen width and screen height, or leave both blank for automatic sizing."
    return 1
  fi
  clipboard_poll_ms="$(zenity --entry --title="$APP_NAME settings" --text="Clipboard poll interval (ms)" --entry-text="$clipboard_poll_ms" || true)"
  [[ -n "$clipboard_poll_ms" ]] || return 1
  if ! is_integer_in_range "$clipboard_poll_ms" 1 2147483647; then
    zenity --error --text="Clipboard poll interval must be a positive integer."
    return 1
  fi

  local toggles
  toggles="$(zenity --list --checklist --title="$APP_NAME clipboard options" --width=500 --height=250 \
    --column="Enabled" --column="Option" \
    $([[ "$clipboard_enabled" == "true" ]] && printf 'TRUE' || printf 'FALSE') "Enable clipboard sync" \
    $([[ "$clipboard_send_enabled" == "true" ]] && printf 'TRUE' || printf 'FALSE') "Send local clipboard changes" \
    $([[ "$clipboard_force_poll" == "true" ]] && printf 'TRUE' || printf 'FALSE') "Force Wayland polling fallback" \
    --separator='|' || true)"

  clipboard_enabled="false"
  clipboard_send_enabled="false"
  clipboard_force_poll="false"
  [[ "$toggles" == *"Enable clipboard sync"* ]] && clipboard_enabled="true"
  [[ "$toggles" == *"Send local clipboard changes"* ]] && clipboard_send_enabled="true"
  [[ "$toggles" == *"Force Wayland polling fallback"* ]] && clipboard_force_poll="true"

  mpris_player="$(zenity --entry --title="$APP_NAME media keys" --text="MPRIS player name (blank for active player)" --entry-text="$mpris_player" || true)"
  toggles="$(zenity --list --checklist --title="$APP_NAME media keys" --width=500 --height=180 \
    --column="Enabled" --column="Option" \
    $([[ "$mpris_media_keys_enabled" == "true" ]] && printf 'TRUE' || printf 'FALSE') "Dispatch media keys through MPRIS/playerctl" \
    $([[ "$latency_report" == "true" ]] && printf 'TRUE' || printf 'FALSE') "Print input latency report when service stops" \
    --separator='|' || true)"
  mpris_media_keys_enabled="false"
  latency_report="false"
  [[ "$toggles" == *"Dispatch media keys through MPRIS/playerctl"* ]] && mpris_media_keys_enabled="true"
  [[ "$toggles" == *"Print input latency report when service stops"* ]] && latency_report="true"

  write_config "$host" "$key" "$key_file" "$secret_id" "$machine_name" "$port" "$auto_connect_enabled" "$reconnect_initial_backoff_ms" "$reconnect_max_backoff_ms" "$reconnect_idle_retry_ms" "$clipboard_enabled" "$clipboard_send_enabled" "$clipboard_force_poll" "$clipboard_poll_ms" "$screen_width" "$screen_height" "$mpris_media_keys_enabled" "$mpris_player" "$latency_report" "$secret_key_name"
  if [[ -n "$preset_host" ]]; then
    saved_message="Saved $host as the configured Windows host in $CONFIG_PATH"
  else
    saved_message="Saved settings to $CONFIG_PATH"
  fi
  if [[ -n "$cleanup_secret_id" ]]; then
    if clear_secret_service_key "$cleanup_secret_id"; then
      saved_message+=$'\nCleared the previous Secret Service entry.'
    else
      saved_message+=$'\nThe previous Secret Service entry could not be cleared.'
    fi
  fi
  zenity --info --text="$saved_message"
  offer_service_restart_if_active "Settings were updated while the background service is running."
}

edit_connection_behavior() {
  local host key key_file secret_id secret_key_name machine_name port screen_width screen_height auto_connect_enabled reconnect_initial_backoff_ms reconnect_max_backoff_ms reconnect_idle_retry_ms clipboard_enabled clipboard_send_enabled clipboard_force_poll clipboard_poll_ms mpris_media_keys_enabled mpris_player latency_report
  local toggles summary_text

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

  summary_text="$(connection_behavior_summary "$auto_connect_enabled" "$reconnect_initial_backoff_ms" "$reconnect_max_backoff_ms" "$reconnect_idle_retry_ms")"
  toggles="$(zenity --list --checklist --title="$APP_NAME connection behavior" --width=560 --height=220 \
    --text="$summary_text" \
    --column="Enabled" --column="Option" \
    $([[ "$auto_connect_enabled" == "true" ]] && printf 'TRUE' || printf 'FALSE') "Automatically reconnect to the configured Windows host" \
    --separator='|' || true)"

  auto_connect_enabled="false"
  [[ "$toggles" == *"Automatically reconnect to the configured Windows host"* ]] && auto_connect_enabled="true"

  reconnect_initial_backoff_ms="$(zenity --entry --title="$APP_NAME connection behavior" --text="Initial retry delay in milliseconds" --entry-text="$reconnect_initial_backoff_ms" || true)"
  [[ -n "$reconnect_initial_backoff_ms" ]] || return 1
  reconnect_max_backoff_ms="$(zenity --entry --title="$APP_NAME connection behavior" --text="Maximum retry delay in milliseconds" --entry-text="$reconnect_max_backoff_ms" || true)"
  [[ -n "$reconnect_max_backoff_ms" ]] || return 1
  reconnect_idle_retry_ms="$(zenity --entry --title="$APP_NAME connection behavior" --text="Idle retry interval in milliseconds once the peer stays offline" --entry-text="$reconnect_idle_retry_ms" || true)"
  [[ -n "$reconnect_idle_retry_ms" ]] || return 1

  if ! is_integer_in_range "$reconnect_initial_backoff_ms" 1 2147483647; then
    zenity --error --text="Initial retry delay must be a positive integer."
    return 1
  fi
  if ! is_integer_in_range "$reconnect_max_backoff_ms" 1 2147483647; then
    zenity --error --text="Maximum retry delay must be a positive integer."
    return 1
  fi
  if ! is_integer_in_range "$reconnect_idle_retry_ms" 1 2147483647; then
    zenity --error --text="Idle retry interval must be a positive integer."
    return 1
  fi
  if (( reconnect_initial_backoff_ms > reconnect_max_backoff_ms )); then
    zenity --error --text="Initial retry delay cannot exceed the maximum retry delay."
    return 1
  fi
  if (( reconnect_idle_retry_ms < reconnect_max_backoff_ms )); then
    zenity --error --text="Idle retry interval should be greater than or equal to the maximum retry delay."
    return 1
  fi

  write_config "$host" "$key" "$key_file" "$secret_id" "$machine_name" "$port" "$auto_connect_enabled" "$reconnect_initial_backoff_ms" "$reconnect_max_backoff_ms" "$reconnect_idle_retry_ms" "$clipboard_enabled" "$clipboard_send_enabled" "$clipboard_force_poll" "$clipboard_poll_ms" "$screen_width" "$screen_height" "$mpris_media_keys_enabled" "$mpris_player" "$latency_report" "$secret_key_name"
  zenity --info --text="Saved connection behavior to $CONFIG_PATH"
  offer_service_restart_if_active "Connection behavior was updated while the background service is running."
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
Actions=OpenSettings;OpenConnectionBehavior;ShowTrayHelp;ShowStatus;StartService;RestartService;StopService;

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
    choice="$(zenity --list --title="$APP_NAME" --text="$(menu_summary_text)" --width=540 --height=380 \
      --column="Action" \
      "Open settings" \
      "Connection behavior" \
      "Discover peers" \
      "Start background service" \
      "Restart background service" \
      "Stop background service" \
      "Show known peers" \
      "Tray visibility help" \
      "Show service details" \
      "Install desktop entries" \
      "Quit" || true)"

    case "$choice" in
      "Open settings") edit_settings ;;
      "Connection behavior") edit_connection_behavior ;;
      "Discover peers") discover_and_save_peer ;;
      "Start background service") start_session ;;
      "Restart background service") restart_session ;;
      "Stop background service") stop_session ;;
      "Show known peers") show_peers ;;
      "Tray visibility help") show_tray_visibility_help ;;
      "Show service details") show_status ;;
      "Install desktop entries") install_desktop_entry ;;
      ""|"Quit") exit 0 ;;
    esac
  done
}

require_ui

case "${1:-menu}" in
  ""|menu) main_menu ;;
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
    printf 'Usage: %s [menu|settings|connection|discover|peers|tray-help|status|start|restart|stop|tray|install-desktop-entry]\n' "$(basename "${BASH_SOURCE[0]}")"
    ;;
  *)
    zenity --error --text="Unknown action: $1"
    exit 1
    ;;
esac
