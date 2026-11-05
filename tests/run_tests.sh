#!/bin/sh
# ============================================================================
#  Rabta Programming Language
#  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
#  Course : Compiler Construction - Phase 2 (LLVM IR ahead-of-time compiler)
# ============================================================================
#
#  tests/run_tests.sh - run by `make test`.
#
#  Every program is compiled AHEAD OF TIME by ./rabtac into a native
#  executable under build/tests/, and that executable is then run.
#
#  1. examples/NAME.rabta  compiled with clang, must exit 0 and print
#                          exactly examples/expected/NAME.out
#  2. examples/NAME.rabta  same, but built through llc (--llc)
#  3. tests/aot/NAME.rabta Phase 2 code-generation tests; must print
#                          tests/aot/expected/NAME.out and exit with the
#                          `// EXPECT_EXIT:` value (default 0)
#  4. LLVM IR checks       `rabtac -S` output is valid IR (llvm-as, when
#                          installed), defines @main, and every program
#                          with a banda declares @pthread_create
#  5. tests/NAME.rabta     must be rejected:
#       - the exit status (of rabtac for compile-time errors, of the
#         compiled program for runtime errors) must equal `// EXPECT_EXIT:`
#         (default 1)
#       - stderr must contain the text after `// EXPECT:`
#       - for compile-time errors (exit 1) nothing may be printed on stdout
#         and no executable may be produced

RABTAC=./rabtac
OUT=build/tests
pass=0
fail=0

ok()  { pass=$((pass + 1)); printf '  PASS  %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; [ -n "$2" ] && printf '        %s\n' "$2"; }

mkdir -p "$OUT"
tmp_out=$(mktemp)
tmp_err=$(mktemp)
trap 'rm -f "$tmp_out" "$tmp_err"' EXIT

# compile_and_check NAME SOURCE EXPECTED_OUT EXPECTED_EXIT [rabtac options]
compile_and_check() {
    name=$1 src=$2 expected=$3 want_exit=$4
    shift 4
    exe="$OUT/$name"
    rm -f "$exe"
    if ! "$RABTAC" "$@" "$src" -o "$exe" 2>"$tmp_err"; then
        bad "$name" "compile failed: $(head -n 1 "$tmp_err")"
        return
    fi
    timeout 20 "$exe" >"$tmp_out" 2>"$tmp_err"
    status=$?
    if [ "$status" -ne "$want_exit" ]; then
        bad "$name" "exit status $status (expected $want_exit): $(head -n 1 "$tmp_err")"
    elif [ ! -f "$expected" ]; then
        bad "$name" "missing $expected"
    elif ! cmp -s "$tmp_out" "$expected"; then
        bad "$name" "output differs from $expected"
    else
        ok "$name"
    fi
}

echo "== valid programs (examples/, clang pipeline) =="
for prog in examples/*.rabta; do
    name=$(basename "$prog" .rabta)
    compile_and_check "$name" "$prog" "examples/expected/$name.out" 0
done

echo "== valid programs (examples/, llc pipeline) =="
for prog in examples/*.rabta; do
    name=$(basename "$prog" .rabta)
    compile_and_check "$name.llc" "$prog" "examples/expected/$name.out" 0 --llc
done

echo "== code generation tests (tests/aot/) =="
for prog in tests/aot/*.rabta; do
    name=$(basename "$prog" .rabta)
    want_exit=$(sed -n 's|^// EXPECT_EXIT: ||p' "$prog" | head -n 1)
    [ -z "$want_exit" ] && want_exit=0
    compile_and_check "aot.$name" "$prog" "tests/aot/expected/$name.out" "$want_exit"
done

echo "== LLVM IR checks (rabtac -S) =="
LLVM_AS=$(command -v llvm-as || true)
for prog in examples/*.rabta tests/aot/*.rabta; do
    name=$(basename "$prog" .rabta)
    ll="$OUT/$name.ll"
    if ! "$RABTAC" -S "$prog" -o "$ll" 2>"$tmp_err"; then
        bad "ir.$name" "rabtac -S failed: $(head -n 1 "$tmp_err")"
    elif [ -n "$LLVM_AS" ] && ! "$LLVM_AS" "$ll" -o /dev/null 2>"$tmp_err"; then
        bad "ir.$name" "llvm-as rejected the IR: $(head -n 1 "$tmp_err")"
    elif ! grep -q '^define i32 @main(' "$ll"; then
        bad "ir.$name" "no 'define i32 @main' in $ll"
    elif grep -q '^banda ' "$prog" && ! grep -q '^declare i32 @pthread_create(' "$ll"; then
        bad "ir.$name" "program has a banda but $ll does not declare @pthread_create"
    else
        ok "ir.$name"
    fi
done

echo "== invalid programs (tests/) =="
for prog in tests/*.rabta; do
    name=$(basename "$prog" .rabta)
    want=$(sed -n 's|^// EXPECT: ||p' "$prog" | head -n 1)
    want_exit=$(sed -n 's|^// EXPECT_EXIT: ||p' "$prog" | head -n 1)
    [ -z "$want_exit" ] && want_exit=1
    exe="$OUT/bad.$name"
    rm -f "$exe"

    "$RABTAC" "$prog" -o "$exe" >"$tmp_out" 2>"$tmp_err"
    status=$?
    if [ "$status" -eq 0 ]; then
        # compiled: the error must happen when the program runs
        timeout 20 "$exe" >"$tmp_out" 2>"$tmp_err"
        status=$?
    fi

    if [ -z "$want" ]; then
        bad "$name" "no // EXPECT: line in $prog"
    elif [ "$status" -ne "$want_exit" ]; then
        bad "$name" "expected exit $want_exit, got $status"
    elif ! grep -qF -- "$want" "$tmp_err"; then
        bad "$name" "expected message not found: $want"
    elif [ "$want_exit" -eq 1 ] && [ -s "$tmp_out" ]; then
        bad "$name" "rabtac printed output although the program should be rejected"
    elif [ "$want_exit" -eq 1 ] && [ -e "$exe" ]; then
        bad "$name" "an executable was produced for a rejected program"
    else
        ok "$name  ->  $(head -n 1 "$tmp_err" | sed 's|^[^:]*:||')"
    fi
done

echo
echo "Results: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
