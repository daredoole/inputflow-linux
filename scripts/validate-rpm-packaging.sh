#!/usr/bin/env bash
set -euo pipefail

spec_file="packaging/rpm/inputflow.spec"
cleanup_paths=()
required_files=(
    "README.md"
    "LICENSE"
    "mwb-desktop-ui.sh"
    "assets/icons/inputflow-desktop.svg"
    "packaging/README.md"
    "packaging/usr/lib/sysusers.d/mwb-client.conf"
    "packaging/usr/lib/modules-load.d/mwb-client-uinput.conf"
    "packaging/usr/lib/udev/rules.d/70-mwb-client-uinput.rules"
    "packaging/usr/lib/systemd/user/mwb-client.service"
    "packaging/usr/share/applications/inputflow.desktop"
    "scripts/inputflow-diagnostics-bundle.sh"
)

fail() {
    echo "rpm packaging validation: $*" >&2
    exit 1
}

cleanup() {
    local path
    for path in "${cleanup_paths[@]}"; do
        rm -rf "$path"
    done
}

write_source_file_list() {
    local output_file="$1"
    find . \
        -path ./.git -prune -o \
        -path './build*' -prune -o \
        -path './cmake-build*' -prune -o \
        -path './.cache' -prune -o \
        -type f -print0 >"$output_file"
}

run_quiet() {
    local label="$1"
    shift
    local log_file
    log_file="$(mktemp)"
    cleanup_paths+=("$log_file")
    if ! "$@" >"$log_file" 2>&1; then
        echo "rpm packaging validation: $label failed" >&2
        tail -n 80 "$log_file" >&2
        return 1
    fi
}

trap cleanup EXIT

[[ -f "$spec_file" ]] || fail "missing $spec_file"

for path in "${required_files[@]}"; do
    [[ -f "$path" ]] || fail "missing referenced file: $path"
done

grep -Eq '^Name:[[:space:]]+inputflow$' "$spec_file" || fail "spec package name must remain inputflow"
grep -Eq '^License:[[:space:]]+GPL-3.0-only$' "$spec_file" || fail "spec license must be GPL-3.0-only"
grep -Eq '^BuildRequires:[[:space:]]+systemd-rpm-macros$' "$spec_file" || fail "spec must build with systemd RPM macros"
grep -Eq '^Requires\(preun\):[[:space:]]+systemd$|%\{\?systemd_requires\}' "$spec_file" || fail "spec must include systemd preun requirements"
grep -Eq '^Requires\(post\):[[:space:]]+systemd-udev$' "$spec_file" || fail "spec must require udevadm for post scripts"
grep -Eq '^Requires\(postun\):[[:space:]]+systemd-udev$' "$spec_file" || fail "spec must require udevadm for postun scripts"
grep -Eq '^g[[:space:]]+inputflow[[:space:]]+-[[:space:]]+-[[:space:]]+-$' packaging/usr/lib/sysusers.d/mwb-client.conf || fail "sysusers file must create inputflow group"
grep -Eq 'KERNEL=="uinput"' packaging/usr/lib/udev/rules.d/70-mwb-client-uinput.rules || fail "udev rule must target uinput"
grep -Eq 'GROUP="inputflow"' packaging/usr/lib/udev/rules.d/70-mwb-client-uinput.rules || fail "udev rule must use inputflow group"
grep -Eq 'MODE="0660"' packaging/usr/lib/udev/rules.d/70-mwb-client-uinput.rules || fail "udev rule must set mode 0660"
grep -Eq 'static_node=uinput' packaging/usr/lib/udev/rules.d/70-mwb-client-uinput.rules || fail "udev rule must keep a stable uinput node"
grep -Eq '^uinput$' packaging/usr/lib/modules-load.d/mwb-client-uinput.conf || fail "modules-load file must load uinput"
grep -Eq '^ConditionPathExists=%h/\.config/mwb-client/config\.ini$' packaging/usr/lib/systemd/user/mwb-client.service || fail "user service must be gated by config presence"
grep -Eq '^ExecStart=/usr/bin/mwb_client run --config %h/\.config/mwb-client/config\.ini$' packaging/usr/lib/systemd/user/mwb-client.service || fail "user service must run packaged mwb_client with the gated config"
grep -Eq '^NoNewPrivileges=true$' packaging/usr/lib/systemd/user/mwb-client.service || fail "user service must keep NoNewPrivileges hardening"
grep -Eq '^Type=Application$' packaging/usr/share/applications/inputflow.desktop || fail "desktop entry must be an application"
grep -Eq '^Name=InputFlow Controller$' packaging/usr/share/applications/inputflow.desktop || fail "desktop entry must expose InputFlow Controller"
grep -Eq '^Exec=/usr/libexec/inputflow/mwb-desktop-ui\.sh menu$' packaging/usr/share/applications/inputflow.desktop || fail "desktop entry must launch the packaged controller"
grep -Eq '^Icon=inputflow$' packaging/usr/share/applications/inputflow.desktop || fail "desktop entry must use the packaged inputflow icon name"
grep -Eq '^Terminal=false$' packaging/usr/share/applications/inputflow.desktop || fail "desktop entry must not require a terminal"
grep -Eq '^DIAGNOSTICS_BUNDLE_SCRIPT="\$SCRIPT_DIR/scripts/inputflow-diagnostics-bundle\.sh"$' mwb-desktop-ui.sh || fail "desktop UI must resolve diagnostics bundle next to the packaged controller"
bash -n scripts/inputflow-diagnostics-bundle.sh || fail "diagnostics bundle script must pass bash syntax checks"
grep -Eq 'doctor --config' scripts/inputflow-diagnostics-bundle.sh || fail "diagnostics bundle must collect client doctor output"
grep -Eq 'systemctl --user --no-pager status mwb-client\.service' scripts/inputflow-diagnostics-bundle.sh || fail "diagnostics bundle must collect user service status"
grep -Eq 'redacted' scripts/inputflow-diagnostics-bundle.sh || fail "diagnostics bundle must redact config/state content"
grep -Eq '%\{_bindir\}/mwb_client' "$spec_file" || fail "spec must package mwb_client"
grep -Eq '%\{_bindir\}/mwb_tray' "$spec_file" || fail "spec must package mwb_tray"
grep -Eq '%\{_libexecdir\}/inputflow/mwb-desktop-ui\.sh' "$spec_file" || fail "spec must package the desktop controller"
grep -Eq '%\{_libexecdir\}/inputflow/scripts/inputflow-diagnostics-bundle\.sh' "$spec_file" || fail "spec must package diagnostics bundle script"
grep -Eq '%\{_userunitdir\}/mwb-client\.service' "$spec_file" || fail "spec must package mwb-client.service"
grep -Eq '%\{_sysusersdir\}/mwb-client\.conf' "$spec_file" || fail "spec must package sysusers integration"
grep -Eq '%\{_udevrulesdir\}/70-mwb-client-uinput\.rules' "$spec_file" || fail "spec must package udev rule"
grep -Eq '%\{_datadir\}/applications/inputflow\.desktop' "$spec_file" || fail "spec must package desktop entry"
grep -Eq '%\{_datadir\}/icons/hicolor/scalable/apps/inputflow\.svg' "$spec_file" || fail "spec must package application icon"
grep -Eq '%sysusers_create[[:space:]]+%\{_sysusersdir\}/mwb-client\.conf' "$spec_file" || fail "spec must create the inputflow group via systemd-sysusers"
grep -Eq '%systemd_user_post[[:space:]]+mwb-client\.service' "$spec_file" || fail "spec must run user-unit post macro"
grep -Eq '%systemd_user_preun[[:space:]]+mwb-client\.service' "$spec_file" || fail "spec must run user-unit preun macro"
grep -Eq '%systemd_user_postun_with_restart[[:space:]]+mwb-client\.service' "$spec_file" || fail "spec must run user-unit postun macro"
grep -Eq 'udevadm control --reload-rules' "$spec_file" || fail "spec must reload udev rules"
grep -Eq 'udevadm trigger --name-match=uinput' "$spec_file" || fail "spec must retrigger /dev/uinput"
grep -Eq 'install -Dpm0755 mwb-desktop-ui\.sh .+%\{_libexecdir\}/inputflow/mwb-desktop-ui\.sh' "$spec_file" || fail "spec must install the desktop controller executable"
grep -Eq 'install -Dpm0755 scripts/inputflow-diagnostics-bundle\.sh .+%\{_libexecdir\}/inputflow/scripts/inputflow-diagnostics-bundle\.sh' "$spec_file" || fail "spec must install the diagnostics bundle executable"
grep -Eq 'install -Dpm0644 packaging/usr/share/applications/inputflow\.desktop .+%\{_datadir\}/applications/inputflow\.desktop' "$spec_file" || fail "spec must install the desktop entry"
grep -Eq 'install -Dpm0644 assets/icons/inputflow-desktop\.svg .+%\{_datadir\}/icons/hicolor/scalable/apps/inputflow\.svg' "$spec_file" || fail "spec must install the application icon"

if command -v desktop-file-validate >/dev/null 2>&1; then
    run_quiet "desktop-file-validate" desktop-file-validate packaging/usr/share/applications/inputflow.desktop
fi

if grep -Eq '/usr/bin/inputflow|inputflow-client\.service|inputflow\.service' "$spec_file"; then
    fail "spec introduced an inputflow command or service alias before the documented compatibility pass"
fi

if command -v rpmspec >/dev/null 2>&1; then
    parsed_spec="$(mktemp)"
    cleanup_paths+=("$parsed_spec")
    rpmspec --parse "$spec_file" >"$parsed_spec"
    if grep -Eq 'systemd-sysusers' "$parsed_spec"; then
        grep -Eq 'systemd-sysusers[[:space:]].*mwb-client\.conf' "$parsed_spec" || fail "parsed spec does not create the inputflow group"
    fi
    grep -Eq 'udevadm control --reload-rules' "$parsed_spec" || fail "parsed spec does not reload udev rules"
    grep -Eq 'udevadm trigger --name-match=uinput' "$parsed_spec" || fail "parsed spec does not retrigger /dev/uinput"
fi

if command -v rpmbuild >/dev/null 2>&1; then
    run_quiet "rpmbuild --nobuild" rpmbuild --nobuild "$spec_file"
    if [[ "${MWB_VALIDATE_RPM_BUILD:-0}" == "1" ]]; then
        package_name="$(sed -nE 's/^Name:[[:space:]]+//p' "$spec_file" | head -n 1)"
        package_version="$(sed -nE 's/^Version:[[:space:]]+//p' "$spec_file" | head -n 1)"
        [[ -n "$package_name" && -n "$package_version" ]] || fail "could not read package name/version"

        topdir="$(mktemp -d)"
        cleanup_paths+=("$topdir")
        mkdir -p "$topdir"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

        file_list="$(mktemp)"
        cleanup_paths+=("$file_list")
        write_source_file_list "$file_list"
        tar --null --files-from "$file_list" \
            --transform "s#^\\./#${package_name}-${package_version}/#" \
            -czf "$topdir/SOURCES/${package_name}-${package_version}.tar.gz"

        run_quiet "rpmbuild -bb" rpmbuild -bb --define "_topdir $topdir" "$spec_file"
    fi
fi

echo "rpm packaging validation passed"
