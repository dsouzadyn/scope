#!/usr/bin/env bash
set -euo pipefail

DIST_DIR="${1:-dist}"
APP_DIR="${DIST_DIR}/SCOPE.app"
BIN="${APP_DIR}/Contents/MacOS/scope"
PLIST="${APP_DIR}/Contents/Info.plist"
FW_DIR="${APP_DIR}/Contents/Frameworks"
FIX_DIR="${APP_DIR}/Contents/Resources/fixtures"

echo "[VERIFY] Checking SCOPE.app bundle structure..."
test -x "${BIN}"
test -f "${PLIST}"
test -d "${FW_DIR}"
test -d "${FIX_DIR}"

echo "[VERIFY] Validating Info.plist..."
plutil -lint "${PLIST}"
grep -q "com.dylandsouza.scope" "${PLIST}"
grep -q "NSHighResolutionCapable" "${PLIST}"

echo "[VERIFY] Checking dylib linkage (no leaked Homebrew paths except liblldb)..."
check_no_leaked_brew() {
    local file="$1"
    local leaked
    leaked="$(otool -L "${file}" | tail -n +2 | awk '{print $1}' | grep -E '^(/opt/homebrew/|/usr/local/)' | grep -v 'liblldb' || true)"
    if [ -n "${leaked}" ]; then
        echo "FAIL: Leaked non-bundled Homebrew dylib in ${file}:" >&2
        echo "${leaked}" >&2
        exit 1
    fi
}

check_no_leaked_brew "${BIN}"
for dylib in "${FW_DIR}"/*.dylib; do
    check_no_leaked_brew "${dylib}"
done

echo "[VERIFY] Checking all bundled fixtures..."
for name in step_basics stl_containers memory_watch multithread crash_autopsy hft_orderbook; do
    test -x "${FIX_DIR}/${name}"
done

echo "[VERIFY] Verifying deep strict ad-hoc code signature..."
codesign --verify --deep --strict "${APP_DIR}"

echo "[VERIFY] Verifying SHA256 checksums..."
(
    cd "${DIST_DIR}"
    shasum -a 256 -c SHA256SUMS.txt
)

echo "ALL MACOS PACKAGE INTEGRITY CHECKS PASSED!"
