#!/usr/bin/env bash
#
# Build the libFuzzer harnesses in devtools/fuzz/ and run them.
#
# Two modes, because they answer different questions:
#
#   scripts/fuzz.sh                  the regression pass CI runs. Every seed once, then a fixed
#                                    number of mutations from a fixed seed - deterministic, a
#                                    few seconds, and red only for a bug that is really there.
#   scripts/fuzz.sh --time 600       an actual fuzzing session, ten minutes per target. This is
#                                    the one that finds things; run it when the parser or the
#                                    session's decode paths change.
#
# Findings land in build/fuzz/findings/ as one file per crash, which is also the reproducer:
# ./build/fuzz/debug/devtools/meshclient_fuzz_session build/fuzz/findings/crash-<hash>
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_ROOT="${BUILD_ROOT:-build/fuzz}"
BUILD_DIR="${BUILD_ROOT}/debug"
CORPUS_DIR="${BUILD_ROOT}/corpus"
FINDINGS_DIR="${BUILD_ROOT}/findings"
RUNS=20000
MAX_TOTAL_TIME=0
TARGETS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --time)
            MAX_TOTAL_TIME="${2:?--time needs seconds}"
            shift 2
            ;;
        --runs)
            RUNS="${2:?--runs needs a count}"
            shift 2
            ;;
        -h|--help)
            sed -n '2,17p' "$0"
            exit 0
            ;;
        *)
            TARGETS+=("$1")
            shift
            ;;
    esac
done

if [[ ${#TARGETS[@]} -eq 0 ]]; then
    TARGETS=(stream_framing session)
fi

# clang only: libFuzzer is a clang runtime, and CMake says so too if this is missed.
CC="${CC:-clang}"
if ! command -v "${CC}" >/dev/null 2>&1; then
    echo "fuzzing needs clang (set CC, or use ./scripts/docker.sh scripts/fuzz.sh)" >&2
    exit 1
fi

# ASan alongside, because a fuzzer without one reports only the crashes bad enough to fault.
CC="${CC}" cmake -S . -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DMESHCLIENT_ENABLE_FUZZERS=ON \
    -DMESHCLIENT_ENABLE_ASAN=ON \
    -DMESHCLIENT_ENABLE_UBSAN=ON \
    -DBUILD_TESTING=OFF
cmake --build "${BUILD_DIR}" --target meshclient_fuzz_seeds \
    "${TARGETS[@]/#/meshclient_fuzz_}"

mkdir -p "${FINDINGS_DIR}"
"${BUILD_DIR}/devtools/meshclient_fuzz_seeds" "${CORPUS_DIR}"

status=0
for target in "${TARGETS[@]}"; do
    binary="${BUILD_DIR}/devtools/meshclient_fuzz_${target}"
    corpus="${CORPUS_DIR}/${target}"
    echo
    echo "=== ${target}: the corpus, once each ==="
    # No mutation: every seed is run as it stands. This is the regression half - a seed that
    # once found a bug is a test case forever, and this pass is what re-runs it.
    "${binary}" -runs=0 "${corpus}" || status=$?

    echo
    if [[ "${MAX_TOTAL_TIME}" -gt 0 ]]; then
        echo "=== ${target}: fuzzing for ${MAX_TOTAL_TIME}s ==="
        "${binary}" -max_total_time="${MAX_TOTAL_TIME}" -max_len=1024 \
            -artifact_prefix="${FINDINGS_DIR}/" "${corpus}" || status=$?
    else
        echo "=== ${target}: ${RUNS} mutations from a fixed seed ==="
        # -seed pins the mutator so this is the same work every time it runs: a red here is a
        # bug in the diff, not a fuzzer that happened to get lucky on somebody's pull request.
        "${binary}" -runs="${RUNS}" -seed=1 -max_len=1024 \
            -artifact_prefix="${FINDINGS_DIR}/" "${corpus}" || status=$?
    fi
done

echo
if [[ "${status}" -ne 0 ]]; then
    echo "fuzzing found something; the reproducer is in ${FINDINGS_DIR}/" >&2
    exit "${status}"
fi
echo "no findings."
