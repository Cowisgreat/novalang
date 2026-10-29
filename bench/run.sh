#!/usr/bin/env bash
# Compares NovaLang with C, and measures what runtime safety costs.
#   bench/run.sh [path-to-novac]
#
# C is built with clang (same LLVM backend as novac, override with CC=...), and both
# target the host CPU: C gets -march=native, novac always does.
# "unchecked" drops all runtime checks; "no-bce" keeps every bounds check, i.e.
# disables the front end's bounds-check elimination.
set -euo pipefail
cd "$(dirname "$0")"
NOVAC=${1:-../build/novac}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

best_of_3() {  # fastest wall-clock time of three runs, in seconds
  local best=
  for _ in 1 2 3; do
    local start end t
    start=$(date +%s.%N); "$@" > /dev/null; end=$(date +%s.%N)
    t=$(echo "$end - $start" | bc)
    if [ -z "$best" ] || (( $(echo "$t < $best" | bc) )); then best=$t; fi
  done
  printf "%.2fs" "$best"
}

printf "%-8s | %9s | %9s %11s | %9s %11s %11s\n" "" "clang -O2" "Nova -O2" "unchecked" "Nova -O0" "no-bce" "unchecked"
for b in nbody sum pairs; do
  "${CC:-clang}" -O2 -march=native "$b.c" -o "$OUT/$b.c" -lm
  for cfg in "O2:-O2" "O2u:-O2 --unchecked" "O0:-O0" "O0n:-O0 --no-bce" "O0u:-O0 --unchecked"; do
    # shellcheck disable=SC2086
    "$NOVAC" "$b.nov" ${cfg#*:} -o "$OUT/$b.${cfg%%:*}"
  done
  printf "%-8s | %9s | %9s %11s | %9s %11s %11s\n" "$b" "$(best_of_3 "$OUT/$b.c")" \
    "$(best_of_3 "$OUT/$b.O2")" "$(best_of_3 "$OUT/$b.O2u")" \
    "$(best_of_3 "$OUT/$b.O0")" "$(best_of_3 "$OUT/$b.O0n")" "$(best_of_3 "$OUT/$b.O0u")"
done
