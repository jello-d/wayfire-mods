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

# THE BUILD ID: stable for one checkout, different when a patch changes, and
# what `check` compares the installed prefix against.
_id() { env WBM_ROOT="$1" sh "$1/libexec/build-wayfire" --build-id; }
_a=$(_id "$HERE")
[ -n "$_a" ] && [ "$_a" = "$(_id "$HERE")" ] || fail "build id not stable"
mkdir -p "$T/r"
( cd "$HERE" && tar -cf - refs libexec patches plugins ) | tar -xf - -C "$T/r"
[ "$(_id "$T/r")" = "$_a" ] || fail "an identical copy has another build id"
_p=$(ls "$T/r/patches/"*.patch | head -n 1)
printf '\n' >> "$_p"
[ "$(_id "$T/r")" != "$_a" ] || fail "editing ${_p##*/} left the build id"

# check against a fake prefix: missing, matching and stale ids
mkdir -p "$T/p/bin" "$T/p/share/wayfire-mods"
printf '#!/bin/sh\n' > "$T/p/bin/wayfire"; chmod +x "$T/p/bin/wayfire"
_chk() {
  env HOME="$T" WF_PREFIX="$T/p" WBM_CACHE="$T/c" sh "$HERE/setup.sh" check \
    2>&1 || :
}
_chk | grep -q 'no build id' || fail "check missed a prefix with no build id"
printf '%s\n' "$_a" > "$T/p/share/wayfire-mods/build-id"
_chk | grep -q "OK.*installed build is this checkout's" \
  || fail "check did not accept this checkout's own build id"
printf 'stale-0\n' > "$T/p/share/wayfire-mods/build-id"
_chk | grep -q "FAIL.*installed build is NOT this checkout's" \
  || fail "check accepted a stale build id"
pass
