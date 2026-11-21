#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build-cov}"
OUT_DIR="${2:-coverage}"
MIN_LINES="${MIN_LINES:-75}"

mkdir -p "${OUT_DIR}"
rm -f "${OUT_DIR}"/*.profraw "${OUT_DIR}/scope.profdata" "${OUT_DIR}/coverage.info"
rm -rf "${OUT_DIR}/html"

echo "[COV] Running instrumented test binaries..."
LLVM_PROFILE_FILE="${OUT_DIR}/unit-%p.profraw" \
    "${BUILD_DIR}/scope_unit_tests" "$(pwd)/fixtures"

LLVM_PROFILE_FILE="${OUT_DIR}/engine-%p.profraw" \
    "${BUILD_DIR}/scope_engine_test" "${BUILD_DIR}/fixtures"

echo "[COV] Merging LLVM coverage profiles..."
xcrun llvm-profdata merge -sparse "${OUT_DIR}"/*.profraw -o "${OUT_DIR}/scope.profdata"

OBJECTS=(
    "${BUILD_DIR}/scope_unit_tests"
    -object "${BUILD_DIR}/scope_engine_test"
)
SRC_FILTER="src/engine/"

echo ""
echo "=============================================================================="
echo " SCOPE ENGINE CODE COVERAGE REPORT"
echo "=============================================================================="
REPORT="$(xcrun llvm-cov report \
    "${OBJECTS[@]}" \
    -instr-profile="${OUT_DIR}/scope.profdata" \
    "${SRC_FILTER}")"
echo "${REPORT}"
echo "=============================================================================="

# Export LCOV & HTML reports
xcrun llvm-cov export \
    "${OBJECTS[@]}" \
    -instr-profile="${OUT_DIR}/scope.profdata" \
    -format=lcov \
    "${SRC_FILTER}" > "${OUT_DIR}/coverage.info"

xcrun llvm-cov show \
    "${OBJECTS[@]}" \
    -instr-profile="${OUT_DIR}/scope.profdata" \
    -format=html \
    -output-dir="${OUT_DIR}/html" \
    "${SRC_FILTER}"

# Parse TOTAL line coverage percentage (column 10 in llvm-cov report TOTAL row)
LINE_PCT="$(echo "${REPORT}" | awk '/^TOTAL/ { gsub(/%/, "", $10); print $10 }')"
echo "[COV] Total engine line coverage: ${LINE_PCT}% (minimum gate: ${MIN_LINES}%)"
echo "[COV] LCOV: ${OUT_DIR}/coverage.info"
echo "[COV] HTML: ${OUT_DIR}/html/index.html"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "## SCOPE Engine Code Coverage (${LINE_PCT}% lines)"
        echo '```text'
        echo "${REPORT}"
        echo '```'
    } >> "${GITHUB_STEP_SUMMARY}"
fi

awk -v got="${LINE_PCT}" -v min="${MIN_LINES}" 'BEGIN { exit (got + 0 >= min + 0) ? 0 : 1 }' || {
    echo "FAIL: Line coverage ${LINE_PCT}% is below minimum threshold ${MIN_LINES}%" >&2
    exit 1
}
