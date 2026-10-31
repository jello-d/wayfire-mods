#!/bin/sh
# setup.t - dispatch + audit, no build. version reports the pins; check with no
# prefix with no wayfire reports [FAIL] not-installed and exits non-zero. (The
# real build is setup.sh verify: heavy, out of the fast suite. The sudo-safe
# install skip needs a live sudo context, not unit-testable here.)
. "$(dirname "$0")/harness_lib"
harness_init setup
sh "$HERE/setup.sh" version | grep -q 'wlroots ' \
  || fail "version does not report the upstream pins"
_out=$(env HOME="$T" WF_PREFIX="$T/p" WBM_CACHE="$T/c" \
  sh "$HERE/setup.sh" check 2>&1) && fail "check passed with no wayfire built"
printf '%s\n' "$_out" | grep -q 'not installed' \
  || fail "check did not report the missing wayfire"
# uninstall removes trees named by overridable variables, so a shallow or
# relative one must be refused before anything is removed.
mkdir -p "$T/keep"
# (Empty is NOT a case: it means the default /opt/wayfire, the live prefix.)
for _bad in keep / relative/x; do
  ( cd "$T" && env HOME="$T" WF_PREFIX="$_bad" WBM_CACHE="$T/c" \
      sh "$HERE/setup.sh" uninstall >/dev/null 2>&1 ) \
    && fail "uninstall accepted WF_PREFIX=[$_bad]"
done
[ -d "$T/keep" ] || fail "a refused uninstall still removed something"
sh "$HERE/setup.sh" bogus >/dev/null 2>&1 && fail "unknown verb did not error" \
  || :
pass
