# Stable release checklist

Run `scripts/release-gate.sh` from a clean release candidate.
The gate stages the portable archive, unsigned Android release APK, changelog,
CycloneDX SBOM, provenance statement, and `SHA256SUMS` under
`build-release-gate/release/`.

- All required CI jobs and the local release gate pass.
- No unresolved P0/P1 bugs or known critical/high vulnerabilities remain.
- Medium security findings have an owner, disposition, and target release.
- Fedora RPM and portable archive pass clean install, upgrade, launch, and
  removal smoke tests.
- GNOME/KDE on X11/Wayland and Android API 26/current are smoke-tested.
- Pairing, reconnect, suspend/resume, tray lifecycle, settings persistence,
  clipboard, topology, notification sync, and permission revocation pass.
- Diagnostics are previewed and confirmed free of secrets and personal/device
  identifiers.
- Release archives, APK, checksums, SBOM/provenance, changelog, compatibility,
  privacy, and security documentation are published together.
- The staged Android APK is unsigned by design. Sign it with the protected
  release key, verify the signature, and replace the unsigned artifact before
  publication.
- The release candidate completes a seven-day reconnect and resource-usage soak.
