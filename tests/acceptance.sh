#!/usr/bin/env bash
# The acceptance tests 2 to 7 of README section 8. Each command is printed before its output, then its exit
# code; the last line counts the checks that came out as expected, and the script's own exit code is 0 only
# when all of them did. Build quill.exe first with build.bat (that is test 1).
# Tests 3, 4 and 6 read the article folders of the planted kit, which is not part of this repository. The
# kit's folder comes from tests/paths.local.env (copy tests/paths.example.env; git ignores the copy), and an
# environment variable of the same name overrides the file.
# Usage (Git Bash, from anywhere):  bash tests/acceptance.sh > acceptance.txt 2>&1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -f "$ROOT/tests/paths.local.env" ]; then
  while IFS='=' read -r k v || [ -n "$k" ]; do
    v="${v%$'\r'}"
    case "$k" in ''|'#'*) continue ;; esac
    [ -z "${!k:-}" ] && export "$k=$v"
  done < "$ROOT/tests/paths.local.env"
fi
case "${QUILL_KIT:-/path/to/}" in
  /path/to/*) echo "set QUILL_KIT in tests/paths.local.env (see tests/paths.example.env)" >&2; exit 2 ;;
esac
Q="${QUILL_EXE:-$ROOT/quill.exe}"
A="$QUILL_KIT/articles"
cd "$ROOT" || exit 2
total=0
bad=0
out=""

count() {  # count OK WHAT: one check, as expected or not
  total=$((total + 1))
  if [ "$1" -eq 0 ]; then bad=$((bad + 1)); echo "NOT AS EXPECTED: $2"; fi
}

run() {  # run EXPECTED_EXIT ARGS...: print the command, its output and its exit code
  local want="$1"
  shift
  echo "\$ quill $*"
  out="$("$Q" "$@" 2>&1)"
  local rc=$?
  printf '%s\n' "$out"
  echo "exit=$rc"
  count "$([ "$rc" -eq "$want" ] && echo 1 || echo 0)" "exit code $rc, expected $want"
  echo
}

echo "=== 2 the self-test"
run 0 --selftest
count "$(printf '%s\n' "$out" | grep -q 'SELFTEST: ALL PASS' && echo 1 || echo 0)" "the self-test did not say ALL PASS"

echo "=== 3 check-kit on the nine recurrent articles"
for n in R0_memoryless R1_plain R1_rewired R2_quotient R3_overcomplete R4_two_bits R5_redundant R9_session C1_even; do
  run 0 check-kit "$A/$n"
done

echo "=== 4 the R1_plain specification, built axis-aligned, against the kit's R1_plain"
run 0 check-kit "$A/R1_plain" --spec specs/R1_plain.spec
run 0 prove specs/R1_plain.spec --equiv "$A/R1_plain"

echo "=== 5 the three planted faults, each refused (exit code 1)"
for s in fault_ramp fault_two_setters fault_never_cleared; do
  run 1 prove "specs/$s.spec"
done

echo "=== 6 speed on the kit's R1_plain, its own tokens a thousand times over (at least 2,000,000 steps per second)"
for i in 1 2 3; do
  run 0 run "$A/R1_plain" --tokens-file "$A/R1_plain/tokens.i32" --repeat 1000 --show 0
  sps="$(printf '%s\n' "$out" | sed -n 's/^ *\([0-9,]*\) steps per second.*/\1/p' | tr -d ',')"
  count "$([ -n "$sps" ] && [ "$sps" -ge 2000000 ] && echo 1 || echo 0)" "speed '$sps' steps per second"
done
echo

echo "=== 7 a counter modulo 3 with reset: 3 reachable states, grade proved"
run 0 prove specs/counter_mod3.spec
count "$(printf '%s\n' "$out" | grep -q 'reachable    3 states' && printf '%s\n' "$out" | grep -q 'ok, grade proved' && echo 1 || echo 0)" "not 3 states with grade proved"

echo "acceptance: $((total - bad)) of $total checks as expected"
[ "$bad" -eq 0 ]
