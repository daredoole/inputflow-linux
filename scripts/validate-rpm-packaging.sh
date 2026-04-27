#!/usr/bin/env bash
set -euo pipefail

spec_file="packaging/rpm/inputflow.spec"
cleanup_paths=()
required_files=(
    "README.md"
    "LICENSE"
    "packaging/README.md"
    "packaging/usr/lib/sysusers.d/mwb-client.conf"
    "packaging/usr/lib/modules-load.d/mwb-client-uinput.conf"
    "packaging/usr/lib/udev/rules.d/70-mwb-client-uinput.rules"
    "packaging/usr/lib/systemd/user/mwb-client.service"
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
grep -Eq '%\{_bindir\}/mwb_client' "$spec_file" || fail "spec must package mwb_client"
grep -Eq '%\{_userunitdir\}/mwb-client\.service' "$spec_file" || fail "spec must package mwb-client.service"
grep -Eq '%\{_sysusersdir\}/mwb-client\.conf' "$spec_file" || fail "spec must package sysusers integration"
grep -Eq '%\{_udevrulesdir\}/70-mwb-client-uinput\.rules' "$spec_file" || fail "spec must package udev rule"
grep -Eq '%sysusers_create[[:space:]]+%\{_sysusersdir\}/mwb-client\.conf' "$spec_file" || fail "spec must create the inputflow group via systemd-sysusers"
grep -Eq '%systemd_user_post[[:space:]]+mwb-client\.service' "$spec_file" || fail "spec must run user-unit post macro"
grep -Eq 'udevadm control --reload-rules' "$spec_file" || fail "spec must reload udev rules"
grep -Eq 'udevadm trigger --name-match=uinput' "$spec_file" || fail "spec must retrigger /dev/uinput"

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
