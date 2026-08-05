#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="${INPUTFLOW_RELEASE_BUILD_DIR:-$REPO_ROOT/build-release-gate}"

cd "$REPO_ROOT"

echo "[1/9] Repository hygiene"
git diff --check
python3 scripts/audit-public-repo.py --include-untracked
if git ls-files | grep -E '(^|/)(\.env($|\.)|.*\.(pem|p12|pfx|key)|inputflow-diagnostics-.*\.tar\.gz)$'; then
  echo "release gate: tracked secret or diagnostics artifact detected" >&2
  exit 1
fi
if git grep -nE -- 'BEGIN ([A-Z ]+ )?PRIVATE KEY|AKIA[0-9A-Z]{16}' -- \
    ':!tests/test_diagnostics_privacy.sh'; then
  echo "release gate: possible embedded credential detected" >&2
  exit 1
fi

echo "[2/9] Script and privacy checks"
bash -n mwb-desktop-ui.sh scripts/*.sh tests/*.sh
PYTHONPYCACHEPREFIX="$BUILD_DIR/pycache" python3 -m py_compile scripts/generate-release-metadata.py
tests/test_diagnostics_privacy.sh scripts/inputflow-diagnostics-bundle.sh

echo "[3/9] Packaging metadata"
scripts/validate-rpm-packaging.sh

echo "[4/9] Configure release build"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "[5/9] Build Linux targets"
cmake --build "$BUILD_DIR" --parallel

echo "[6/9] Linux regression suite"
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "[7/9] Portable archive"
cmake --build "$BUILD_DIR" --target package
PORTABLE_ARCHIVE="$(find "$BUILD_DIR" -maxdepth 1 -name 'inputflow-*.tar.gz' -print -quit)"
test -n "$PORTABLE_ARCHIVE"
test -n "$(find "$BUILD_DIR" -maxdepth 1 -name 'inputflow-*.tar.gz.sha256' -print -quit)"
tests/test_portable_archive.sh "$PORTABLE_ARCHIVE"

echo "[8/9] Android release build, tests, and lint"
if [[ "${INPUTFLOW_SKIP_ANDROID:-0}" == "1" ]]; then
  echo "Android gate explicitly skipped"
elif [[ -n "${JAVA_HOME:-}" && ! -x "${JAVA_HOME}/bin/java" ]]; then
  env -u JAVA_HOME ./android/gradlew -p android \
    :app:assembleRelease :app:testDebugUnitTest :app:lintRelease \
    --no-daemon --stacktrace
else
  ./android/gradlew -p android \
    :app:assembleRelease :app:testDebugUnitTest :app:lintRelease \
    --no-daemon --stacktrace
fi

echo "[9/9] SBOM, provenance, and release checksums"
if [[ "${INPUTFLOW_SKIP_ANDROID:-0}" == "1" ]]; then
  echo "Release metadata skipped because the Android artifact was skipped"
else
  ANDROID_APK="${INPUTFLOW_ANDROID_APK:-android/app/build/outputs/apk/release/app-release-unsigned.apk}"
  METADATA_ARGS=(
    --build-dir "$BUILD_DIR"
    --android-apk "$ANDROID_APK"
  )
  if [[ "${INPUTFLOW_REQUIRE_SIGNED_ANDROID:-0}" == "1" ]]; then
    METADATA_ARGS+=(--require-signed-android)
  fi
  python3 scripts/generate-release-metadata.py "${METADATA_ARGS[@]}"
fi

echo "InputFlow release gate passed"
