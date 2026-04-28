# Packaging Artifacts

This directory contains reusable installable files for distribution packages and local
system integration. The tree mirrors the intended `/usr/lib` installation paths so
packagers can copy files directly or map them to distro macros.

## Files

- `usr/lib/sysusers.d/mwb-client.conf`
  Creates the dedicated `inputflow` system group. Use this instead of adding users
  to a broad `input` group.
- `usr/lib/modules-load.d/mwb-client-uinput.conf`
  Loads the `uinput` kernel module at boot.
- `usr/lib/udev/rules.d/70-mwb-client-uinput.rules`
  Grants `/dev/uinput` read/write access to the `inputflow` group and asks udev to
  keep a stable `/dev/uinput` node.
- `usr/lib/systemd/user/mwb-client.service`
  Runs `mwb_client` from a user systemd manager with
  `%h/.config/mwb-client/config.ini`.
- `rpm/inputflow.spec`
  Fedora/RPM packaging skeleton for the InputFlow package. It intentionally keeps
  the installed command and user service named `mwb_client` and
  `mwb-client.service` until a dedicated alias migration is documented.

## Fedora/RPM Notes

Install the files to the standard macro destinations:

- `%{_sysusersdir}/mwb-client.conf`
- `%{_modulesloaddir}/mwb-client-uinput.conf`
- `%{_udevrulesdir}/70-mwb-client-uinput.rules`
- `%{_userunitdir}/mwb-client.service`

Create the group with systemd-sysusers from package scriptlets/macros. Reload udev
rules after installing the rule, and load or trigger `uinput` if the package wants
the device available immediately. Do not add users to `inputflow` automatically;
that remains an administrator or user opt-in step.

For systemd integration, use the distro's user-unit macros where available, such as
Fedora's `%systemd_user_post`, `%systemd_user_preun`, and `%systemd_user_postun`.
Packages should not globally enable the user service by default unless their distro
policy explicitly allows it.

Validate RPM packaging metadata without requiring a full RPM build:

```bash
scripts/validate-rpm-packaging.sh
```

The validator checks referenced packaging files, preserved command/service names,
and scriptlet macros. When `rpmspec` or `rpmbuild` is installed, it also runs
`rpmspec --parse` and `rpmbuild --nobuild` against `rpm/inputflow.spec`.
Fedora CI enables a full RPM build with:

```bash
MWB_VALIDATE_RPM_BUILD=1 scripts/validate-rpm-packaging.sh
```

## User Setup

After package installation, an administrator can opt a desktop user into rootless
input injection:

```bash
sudo usermod -aG inputflow "$USER"
```

The user must log out and back in before the new group membership is visible to
their user systemd manager.

Create the client configuration, then enable the user service for login autostart:

```bash
mwb_client init-config --config ~/.config/mwb-client/config.ini
systemctl --user daemon-reload
systemctl --user enable --now mwb-client.service
```

The packaged user unit is intentionally configuration-gated with
`ConditionPathExists=%h/.config/mwb-client/config.ini`; an enabled service will be
skipped until the user creates that file.

Firewall rules are normally unnecessary for the Linux client when it only initiates
outbound connections to a Windows host. SELinux policy should be kept distro-owned;
do not disable SELinux or apply broad device labels for this service unless a
specific denial is observed and reviewed.
