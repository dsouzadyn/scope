#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-v0.1.0}"
CLEAN_VERSION="${VERSION#v}"
BUILD_DIR="${2:-build}"
DIST_DIR="${3:-dist}"
ARCH="$(uname -m)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APP_DIR="${DIST_DIR}/SCOPE.app"
CONTENTS_DIR="${APP_DIR}/Contents"
MACOS_DIR="${CONTENTS_DIR}/MacOS"
FRAMEWORKS_DIR="${CONTENTS_DIR}/Frameworks"
RESOURCES_DIR="${CONTENTS_DIR}/Resources"

rm -rf "${DIST_DIR}"
mkdir -p "${MACOS_DIR}" "${FRAMEWORKS_DIR}" "${RESOURCES_DIR}/fixtures"

# 1) Copy main executable & sample C++ fixtures
cp "${BUILD_DIR}/scope" "${MACOS_DIR}/scope"
chmod +x "${MACOS_DIR}/scope"

if [ -d "${BUILD_DIR}/fixtures" ]; then
    for bin in "${BUILD_DIR}/fixtures"/*; do
        if [ -f "${bin}" ] && [ -x "${bin}" ]; then
            cp "${bin}" "${RESOURCES_DIR}/fixtures/"
        fi
    done
fi

# 2) Generate native macOS AppIcon.icns
python3 "${SCRIPT_DIR}/generate_icon.py" "${RESOURCES_DIR}/AppIcon.icns"

# 3) Generate macOS Info.plist
cat > "${CONTENTS_DIR}/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleDisplayName</key>
    <string>SCOPE</string>
    <key>CFBundleExecutable</key>
    <string>scope</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
    <key>CFBundleIdentifier</key>
    <string>com.dylandsouza.scope</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>SCOPE</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${CLEAN_VERSION}</string>
    <key>CFBundleVersion</key>
    <string>${CLEAN_VERSION}</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key>
    <true/>
</dict>
</plist>
EOF

# Helper: resolve absolute, @rpath/, or @loader_path/ dylib reference to a real file on disk.
resolve_dep_path() {
    local ref="$1"
    local orig_dir="$2"

    if [ -f "${ref}" ]; then
        echo "${ref}"
        return 0
    fi

    local base
    base="$(basename "${ref}")"

    if [ -n "${orig_dir}" ] && [ -f "${orig_dir}/${base}" ]; then
        echo "${orig_dir}/${base}"
        return 0
    fi

    local dir cand
    for dir in /opt/homebrew/opt/lldb/lib /opt/homebrew/opt/llvm/lib /opt/homebrew/lib /usr/local/opt/lldb/lib /usr/local/opt/llvm/lib /usr/local/lib /opt/homebrew/opt/*/lib /usr/local/opt/*/lib; do
        cand="${dir}/${base}"
        if [ -f "${cand}" ]; then
            echo "${cand}"
            return 0
        fi
    done

    echo "ERROR: Could not locate dynamic library '${ref}' (origin dir: '${orig_dir}')" >&2
    return 1
}

# 4) Recursively bundle ALL non-system dylibs (including liblldb, libLLVM, libclang-cpp,
#    libbrotlicommon, libz3, libzstd, liblzma, glfw, freetype, libpng, tree-sitter)
#    so SCOPE.app is 100% self-contained with ZERO Homebrew prerequisites!
bundle_deps() {
    local target="$1"
    local orig_dir="$2"
    local deps
    deps="$(otool -L "${target}" | tail -n +2 | awk '{print $1}' | grep -E '^(/opt/homebrew/|/usr/local/|@rpath/|@loader_path/)' || true)"
    local dep
    for dep in ${deps}; do
        local base dest resolved resolved_dir
        base="$(basename "${dep}")"
        dest="${FRAMEWORKS_DIR}/${base}"

        if [ "$(basename "${target}")" = "${base}" ]; then
            continue
        fi

        install_name_tool -change "${dep}" "@executable_path/../Frameworks/${base}" "${target}"
        if [ ! -f "${dest}" ]; then
            resolved="$(resolve_dep_path "${dep}" "${orig_dir}")"
            resolved_dir="$(cd "$(dirname "${resolved}")" && pwd -P)"
            cp -L "${resolved}" "${dest}"
            chmod 755 "${dest}"
            install_name_tool -id "@executable_path/../Frameworks/${base}" "${dest}"
            bundle_deps "${dest}" "${resolved_dir}"
        fi
    done
}

install_name_tool -add_rpath "@executable_path/../Frameworks" "${MACOS_DIR}/scope" 2>/dev/null || true
bundle_deps "${MACOS_DIR}/scope" ""

# 5) Sign every bundled dylib & fixture, then sign SCOPE.app
#    Uses Developer ID + Hardened Runtime when CODESIGN_IDENTITY is provided; ad-hoc otherwise.
SIGN_IDENTITY="${CODESIGN_IDENTITY:--}"
SIGN_ARGS=(--force --sign "${SIGN_IDENTITY}")
if [ "${SIGN_IDENTITY}" != "-" ]; then
    SIGN_ARGS+=(--options runtime --timestamp)
fi

for dylib in "${FRAMEWORKS_DIR}"/*.dylib; do
    [ -f "${dylib}" ] && codesign "${SIGN_ARGS[@]}" "${dylib}"
done
for fix in "${RESOURCES_DIR}/fixtures"/*; do
    [ -f "${fix}" ] && codesign "${SIGN_ARGS[@]}" "${fix}"
done
codesign "${SIGN_ARGS[@]}" --deep "${APP_DIR}"

# 6) Create drag-to-Applications .dmg + .zip + SHA256SUMS.txt
DMG_NAME="SCOPE-${VERSION}-macos-${ARCH}.dmg"
ZIP_NAME="SCOPE-${VERSION}-macos-${ARCH}.zip"

DMG_STAGE="${DIST_DIR}/dmg_stage"
mkdir -p "${DMG_STAGE}"
cp -R "${APP_DIR}" "${DMG_STAGE}/SCOPE.app"
ln -s /Applications "${DMG_STAGE}/Applications"

hdiutil create \
    -volname "SCOPE ${VERSION}" \
    -srcfolder "${DMG_STAGE}" \
    -ov \
    -format UDZO \
    "${DIST_DIR}/${DMG_NAME}" >/dev/null
rm -rf "${DMG_STAGE}"

(
    cd "${DIST_DIR}"
    ditto -c -k --sequesterRsrc --keepParent "SCOPE.app" "${ZIP_NAME}"
    shasum -a 256 "${DMG_NAME}" "${ZIP_NAME}" > SHA256SUMS.txt
)

echo "Packaged self-contained macOS release artifacts in ${DIST_DIR}:"
ls -lh "${DIST_DIR}/${DMG_NAME}" "${DIST_DIR}/${ZIP_NAME}" "${DIST_DIR}/SHA256SUMS.txt"
