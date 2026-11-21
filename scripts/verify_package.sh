#!/usr/bin/env bash
set -euo pipefail

DIST_DIR="${1:-dist}"
APP_DIR="${DIST_DIR}/SCOPE.app"
BIN="${APP_DIR}/Contents/MacOS/scope"
PLIST="${APP_DIR}/Contents/Info.plist"
ICON="${APP_DIR}/Contents/Resources/AppIcon.icns"
FW_DIR="${APP_DIR}/Contents/Frameworks"
FIX_DIR="${APP_DIR}/Contents/Resources/fixtures"

echo "[VERIFY] Checking SCOPE.app bundle structure..."
test -x "${BIN}"
test -f "${PLIST}"
test -f "${ICON}"
test -d "${FW_DIR}"
test -d "${FIX_DIR}"

echo "[VERIFY] Validating Info.plist..."
plutil -lint "${PLIST}"
grep -q "com.dylandsouza.scope" "${PLIST}"
grep -q "NSHighResolutionCapable" "${PLIST}"
grep -q "AppIcon" "${PLIST}"

echo "[VERIFY] Checking 100% self-contained dylib linkage (zero Homebrew or unresolved @rpath deps)..."
verify_macho_deps() {
    local file="$1"
    local deps
    deps="$(otool -L "${file}" | tail -n +2 | awk '{print $1}')"
    local dep
    for dep in ${deps}; do
        case "${dep}" in
            /System/Library/*|/usr/lib/*)
                ;;
            @executable_path/../Frameworks/*)
                local base
                base="$(basename "${dep}")"
                if [ ! -f "${FW_DIR}/${base}" ]; then
                    echo "FAIL: Missing bundled framework '${base}' referenced by ${file}" >&2
                    exit 1
                fi
                ;;
            @rpath/*|@loader_path/*)
                echo "FAIL: Unresolved relative dylib '${dep}' in ${file}" >&2
                exit 1
                ;;
            /opt/homebrew/*|/usr/local/*)
                echo "FAIL: Leaked external Homebrew dylib '${dep}' in ${file}" >&2
                exit 1
                ;;
            *)
                echo "FAIL: Unexpected external dylib '${dep}' in ${file}" >&2
                exit 1
                ;;
        esac
    done
}

verify_macho_deps "${BIN}"
for dylib in "${FW_DIR}"/*.dylib; do
    [ -f "${dylib}" ] && verify_macho_deps "${dylib}"
done

echo "[VERIFY] Checking all bundled fixtures..."
for name in step_basics stl_containers memory_watch multithread crash_autopsy hft_orderbook; do
    test -x "${FIX_DIR}/${name}"
done

echo "[VERIFY] Verifying deep strict code signature..."
codesign --verify --deep --strict "${APP_DIR}"

echo "[VERIFY] Verifying release archives & SHA256 checksums..."
if [ -f "${DIST_DIR}/SHA256SUMS.txt" ]; then
    (
        cd "${DIST_DIR}"
        shasum -a 256 -c SHA256SUMS.txt
    )
fi

echo "ALL SELF-CONTAINED MACOS PACKAGE INTEGRITY CHECKS PASSED!"
