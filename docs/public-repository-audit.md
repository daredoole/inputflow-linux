# Public Repository Privacy Audit

Assessment date: 2026-07-28

## Scope

The assessment covered:

- every tracked file;
- every untracked file not excluded by `.gitignore`;
- all 58 commits reachable from local Git references;
- source, documentation, tests, workflow, packaging, and Android files;
- image metadata under the public asset and Android resource trees; and
- Git author metadata.

Build directories and other intentionally ignored local artifacts are not
publishable repository content and were excluded from content-pattern checks.

## Checks

- Gitleaks scan of Git history with findings redacted.
- Gitleaks scan of the current working tree with findings redacted.
- Repository-specific checks for credential/key filenames, private-key
  material, personal Linux and Windows profile paths, hard-coded runtime user
  IDs, non-placeholder email addresses, private/link-local addresses, and MAC
  addresses outside test fixtures.
- Search for known local usernames and hostnames without printing matched
  values.
- EXIF and image-metadata inspection for GPS, author, owner, camera/device,
  serial-number, location, and user-comment fields.
- Validation of README local links and table-of-contents anchors.

## Result

- No credentials or private keys were found in the working tree or Git
  history.
- No known local username or hostname was found in publishable file content.
- No personal home-directory or Windows profile path was found.
- No private/link-local network address or production MAC address was found.
- No risky personal, location, or device metadata was found in public images.
- Documentation addresses, example users, placeholder email domains, protocol
  constants, and test-only fixtures were reviewed as non-personal data.

Git commit objects retain contributor names and author email metadata as normal
project attribution. Some historical authors used direct addresses rather than
GitHub noreply addresses. Those fields are not application secrets or device
data and were not rewritten because changing them would alter published
history and attribution. Contributors who prefer pseudonymous metadata should
configure a verified GitHub noreply address before committing.

## Preventive controls

- `scripts/audit-public-repo.py` blocks common personal/device data and
  sensitive artifacts.
- `scripts/release-gate.sh` runs the audit before building a release.
- `.gitignore` excludes local configuration, credentials, diagnostics,
  signing material, build output, and analysis caches.
- The pull-request template requires confirmation that secrets, real
  addresses, and personal information were not added.
- Diagnostics remain local by default and redact likely personal, device,
  address, secret, and input-event data.

History rewriting is intentionally not performed automatically. If a real
credential is ever discovered, revoke or rotate it first, then coordinate any
history rewrite with maintainers, forks, and existing clones.
