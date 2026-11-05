#!/bin/sh
# ============================================================================
#  Rabta Programming Language
#  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
#  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
# ============================================================================
#
#  tests/run_tests.sh - run by `make test`.
#
#  1. Every examples/NAME.rabta must exit 0 and print exactly
#     examples/expected/NAME.out.
#  2. Every tests/NAME.rabta must be rejected:
#       - its exit status must equal the `// EXPECT_EXIT:` line (default 1)
#       - stderr must contain the text after `// EXPECT:`
#       - for compile-time errors (exit 1) nothing may be printed on stdout,
#         which proves the program was not executed.

RABTA=./rabta
pass=0
fail=0

ok()  { pass=$((pass + 1)); printf '  PASS  %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; [ -n "$2" ] && printf '        %s\n' "$2"; }

tmp_out=$(mktemp)
tmp_err=$(mktemp)
trap 'rm -f "$tmp_out" "$tmp_err"' EXIT

echo "== valid programs (examples/) =="
for prog in examples/*.rabta; do
    name=$(basename "$prog" .rabta)
    expected="examples/expected/$name.out"
    timeout 20 "$RABTA" "$prog" >"$tmp_out" 2>"$tmp_err"
    status=$?
    if [ "$status" -ne 0 ]; then
        bad "$name" "exit status $status: $(head -n 1 "$tmp_err")"
    elif [ ! -f "$expected" ]; then
        bad "$name" "missing $expected"
    elif ! cmp -s "$tmp_out" "$expected"; then
        bad "$name" "output differs from $expected"
    else
        ok "$name"
    fi
done

echo "== invalid programs (tests/) =="
for prog in tests/*.rabta; do
    name=$(basename "$prog" .rabta)
    want=$(sed -n 's|^// EXPECT: ||p' "$prog" | head -n 1)
    want_exit=$(sed -n 's|^// EXPECT_EXIT: ||p' "$prog" | head -n 1)
    [ -z "$want_exit" ] && want_exit=1
    timeout 20 "$RABTA" "$prog" >"$tmp_out" 2>"$tmp_err"
    status=$?
    if [ -z "$want" ]; then
        bad "$name" "no // EXPECT: line in $prog"
    elif [ "$status" -ne "$want_exit" ]; then
        bad "$name" "expected exit $want_exit, got $status"
    elif ! grep -qF -- "$want" "$tmp_err"; then
        bad "$name" "expected message not found: $want"
    elif [ "$want_exit" -eq 1 ] && [ -s "$tmp_out" ]; then
        bad "$name" "program produced output although it should not run"
    else
        ok "$name  ->  $(head -n 1 "$tmp_err" | sed 's|^[^:]*:||')"
    fi
done

echo
echo "Results: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
