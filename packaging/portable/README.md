# Portable Linux archive

The `.tar.gz` release is a relocatable user-space bundle. Extract it, then run:

```bash
./inputflow-*/bin/inputflow-controller
```

The wrapper locates the bundled client and controller without modifying the
system. Input injection still requires administrator-approved `/dev/uinput`
access. Files under `share/inputflow/system-integration/` are reference copies;
review and install them through the distribution package or an administrator
rather than copying them automatically.

The RPM remains the supported system-integrated installation for Fedora. The
portable archive is supported for tested Ubuntu/Fedora desktop sessions but
does not register autostart, udev, sysusers, or desktop menu entries itself.
