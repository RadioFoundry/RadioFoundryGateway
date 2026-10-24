#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# Line and branch coverage of the host tests (clang source-based coverage; on macOS through xcrun).
#   sh host-tests/coverage.sh [build dir] [file filter regex]
# Builds an instrumented copy in the build dir (default build/cov), runs ctest, prints the per-file report
# for common, sdr, rtlsdr and components (tests themselves left out), and writes an HTML report to
# <build dir>/html. Example: sh host-tests/coverage.sh build/cov 'jsonp|modem'
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
out=${1:-$root/build/cov}
mkdir -p "$out"
out=$(cd "$out" && pwd) # absolute: each test runs in its own directory, and writes its profile from there
filter=${2:-.}
if command -v xcrun >/dev/null 2>&1; then
    profdata="xcrun llvm-profdata"
    cov="xcrun llvm-cov"
else
    profdata=llvm-profdata
    cov=llvm-cov
fi

cmake -S "$here" -B "$out" -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping" >/dev/null
cmake --build "$out" -j >/dev/null
rm -rf "$out/prof"
mkdir -p "$out/prof"
LLVM_PROFILE_FILE="$out/prof/%p.profraw" ctest --test-dir "$out" >"$out/ctest.log" 2>&1 \
    || echo "some tests failed ($out/ctest.log); the report covers what ran" >&2
$profdata merge -sparse "$out"/prof/*.profraw -o "$out/all.profdata"

# every test executable built, as -object arguments
set --
for bin in $(find "$out" -maxdepth 2 -type f -perm -u+x ! -name '*.sh' ! -path '*/CMakeFiles/*'); do
    case $(file -b "$bin") in *executable*) set -- "$@" -object "$bin" ;; esac
done
first=$2
shift 2

ignore='(host-tests|/test/|_test\.c|vectors|scenarios)'
# file, lines, branches (llvm-cov's columns 10 and 13); its warnings (functions it can't map) go to
# stderr
$cov report "$first" "$@" -instr-profile="$out/all.profdata" -ignore-filename-regex="$ignore" 2>/dev/null \
    | grep -vE "^(Filename|Files|-)" | grep -E "$filter|TOTAL" \
    | awk 'BEGIN { printf "%-52s %8s %8s\n", "file", "lines", "branches" }
           $10 != "-" { printf "%-52s %8s %8s\n", $1, $10, $13 }'
$cov show "$first" "$@" -instr-profile="$out/all.profdata" -ignore-filename-regex="$ignore" \
    -format=html -output-dir="$out/html" >/dev/null 2>&1
echo "html: $out/html/index.html"
