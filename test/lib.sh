# test/lib.sh - harness for wayfire-mods's shell tests (test/*.t), sourced by
# each. `harness_init <name>` sets HERE (repo root), a scratch dir T (removed on
# exit), and pass/fail/skip. POSIX sh; one with `sh test/<name>.t`, all with
# test/run.
harness_init() {
  TEST_NAME=$1
  HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
  T=$(mktemp -d)
  trap 'rm -rf "$T"' EXIT INT TERM
}
pass() { printf 'ok   %s%s\n' "$TEST_NAME" "${1:+ ($1)}"; }
fail() { printf 'FAIL %s: %s\n' "$TEST_NAME" "$1" >&2; exit 1; }
skip() { printf 'skip %s (%s)\n' "$TEST_NAME" "$1"; exit 0; }
