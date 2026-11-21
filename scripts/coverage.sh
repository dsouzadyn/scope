#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build-cov}"
OUT_DIR="${2:-coverage}"

# Strict multi-metric coverage release gates (override via env if needed)
MIN_LINES="${MIN_LINES:-85}"
MIN_FUNCTIONS="${MIN_FUNCTIONS:-95}"
MIN_REGIONS="${MIN_REGIONS:-82}"
MIN_BRANCHES="${MIN_BRANCHES:-62}"

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

# Parse TOTAL row:
# $1=TOTAL $2=Regions $3=MissedReg $4=RegCov% $5=Funcs $6=MissedFn $7=FnCov%
# $8=Lines $9=MissedLn $10=LnCov% $11=Branches $12=MissedBr $13=BrCov%
REG_PCT="$(echo "${REPORT}" | awk '/^TOTAL/ { gsub(/%/, "", $4); print $4 }')"
FN_PCT="$(echo "${REPORT}" | awk '/^TOTAL/ { gsub(/%/, "", $7); print $7 }')"
LINE_PCT="$(echo "${REPORT}" | awk '/^TOTAL/ { gsub(/%/, "", $10); print $10 }')"
BR_PCT="$(echo "${REPORT}" | awk '/^TOTAL/ { gsub(/%/, "", $13); print $13 }')"

echo "[COV] Gates:"
echo "  Lines:     ${LINE_PCT}% (min ${MIN_LINES}%)"
echo "  Functions: ${FN_PCT}% (min ${MIN_FUNCTIONS}%)"
echo "  Regions:   ${REG_PCT}% (min ${MIN_REGIONS}%)"
echo "  Branches:  ${BR_PCT}% (min ${MIN_BRANCHES}%)"
echo "[COV] LCOV: ${OUT_DIR}/coverage.info"
echo "[COV] HTML: ${OUT_DIR}/html/index.html"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "## SCOPE Engine Code Coverage"
        echo "- **Lines**: ${LINE_PCT}% (gate ≥ ${MIN_LINES}%)"
        echo "- **Functions**: ${FN_PCT}% (gate ≥ ${MIN_FUNCTIONS}%)"
        echo "- **Regions**: ${REG_PCT}% (gate ≥ ${MIN_REGIONS}%)"
        echo "- **Branches**: ${BR_PCT}% (gate ≥ ${MIN_BRANCHES}%)"
        echo '```text'
        echo "${REPORT}"
        echo '```'
    } >> "${GITHUB_STEP_SUMMARY}"
fi

check_gate() {
    local name="$1"
    local got="$2"
    local min="$3"
    awk -v g="${got}" -v m="${min}" 'BEGIN { exit (g + 0 >= m + 0) ? 0 : 1 }' || {
        echo "FAIL: ${name} coverage ${got}% is below minimum gate ${min}%" >&2
        return 1
    }
}

FAILED=0
check_gate "Line"     "${LINE_PCT}" "${MIN_LINES}"     || FAILED=1
check_gate "Function" "${FN_PCT}"   "${MIN_FUNCTIONS}" || FAILED=1
check_gate "Region"   "${REG_PCT}"  "${MIN_REGIONS}"   || FAILED=1
check_gate "Branch"   "${BR_PCT}"   "${MIN_BRANCHES}"  || FAILED=1

if [ "${FAILED}" -ne 0 ]; then
    exit 1
fi
echo "ALL COVERAGE THRESHOLD GATES PASSED!"
