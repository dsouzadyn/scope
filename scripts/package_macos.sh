#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-v0.1.0}"
CLEAN_VERSION="${VERSION#v}"
BUILD_DIR="${2:-build}"
DIST_DIR="${3:-dist}"
ARCH="$(uname -m)"

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

# 2) Generate macOS Info.plist
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

# 3) Bundle non-LLVM Homebrew dylibs (glfw, freetype, libpng, tree-sitter) recursively
#    Keep liblldb linked against Homebrew LLVM so debugserver & entitlements work reliably.
bundle_deps() {
    local target="$1"
    local deps
    deps="$(otool -L "${target}" | tail -n +2 | awk '{print $1}' | grep -E '^(/opt/homebrew/|/usr/local/)' | grep -v 'liblldb' || true)"
    local dep
    for dep in ${deps}; do
        local base dest
        base="$(basename "${dep}")"
        dest="${FRAMEWORKS_DIR}/${base}"
        install_name_tool -change "${dep}" "@executable_path/../Frameworks/${base}" "${target}"
        if [ ! -f "${dest}" ]; then
            cp -L "${dep}" "${dest}"
            chmod 755 "${dest}"
            install_name_tool -id "@executable_path/../Frameworks/${base}" "${dest}"
            bundle_deps "${dest}"
        fi
    done
}

bundle_deps "${MACOS_DIR}/scope"

# 4) Ad-hoc sign the macOS app bundle
codesign --force --deep --sign - "${APP_DIR}"

# 5) Create release archives + SHA256 checksums
ZIP_NAME="SCOPE-${VERSION}-macos-${ARCH}.zip"
TAR_NAME="scope-${VERSION}-macos-${ARCH}.tar.gz"

(
    cd "${DIST_DIR}"
    ditto -c -k --sequesterRsrc --keepParent "SCOPE.app" "${ZIP_NAME}"
    tar -czf "${TAR_NAME}" "SCOPE.app"
    shasum -a 256 "${ZIP_NAME}" "${TAR_NAME}" > SHA256SUMS.txt
)

echo "Packaged macOS release artifacts in ${DIST_DIR}:"
ls -lh "${DIST_DIR}/${ZIP_NAME}" "${DIST_DIR}/${TAR_NAME}" "${DIST_DIR}/SHA256SUMS.txt"
