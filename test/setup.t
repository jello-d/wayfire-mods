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
# THE PORTAL BACKEND: place writes the drop-in only once the binary exists,
# check judges binary, drop-in and distro unit, and what the user manager
# will start is a WARN, never a failure (no apply reaches another account).
_xp() {   # <verb>: setup.sh against the fake prefix, every seam scratch
  env HOME="$T" WF_PREFIX="$T/p" WBM_CACHE="$T/c" \
    XDPW_DROPIN="$T/etc/u/x.service.d/50-wayfire-mods.conf" \
    XDPW_DISTRO_UNIT="$T/unit" XDPW_SYSTEMCTL="$T/sysctl" \
    sh "$HERE/setup.sh" "$1" 2>&1
}
mkdir -p "$T/etc/u"
printf '#!/bin/sh\nprintf "%%s\\n" "${SC_SAYS:-}"\n' > "$T/sysctl"
chmod +x "$T/sysctl"
_xb=$T/p/libexec/xdg-desktop-portal-wlr
_o=$(_xp xdpw-place) && fail "place wrote a drop-in with no binary: $_o"
[ -e "$T/etc/u/x.service.d/50-wayfire-mods.conf" ] \
  && fail "a refused place left a drop-in"
_o=$(_xp check || :)
printf '%s\n' "$_o" | grep -q 'FAIL.*portal backend missing or does not run' \
  || fail "check missed the absent backend: $_o"
printf '%s\n' "$_o" | grep -q 'FAIL.*portal drop-in missing' \
  || fail "check missed the absent drop-in"
printf '%s\n' "$_o" | grep -q 'FAIL.*drop-in attaches to nothing' \
  || fail "check missed the absent distro unit"
mkdir -p "$T/p/libexec"; printf '#!/bin/sh\nexit 0\n' > "$_xb"
chmod +x "$_xb"; : > "$T/unit"
_xp xdpw-place | grep -q 'wrote' || fail "place did not write the drop-in"
grep -qx "ExecStart=$_xb" "$T/etc/u/x.service.d/50-wayfire-mods.conf" \
  || fail "the drop-in does not name the patched binary"
grep -qx 'ExecStart=' "$T/etc/u/x.service.d/50-wayfire-mods.conf" \
  || fail "the drop-in does not reset the distro ExecStart"
[ -z "$(_xp xdpw-place)" ] || fail "a second place was not a no-op"
_o=$(_xp check || :)
for _w in 'OK.*patched portal backend runs' 'OK.*portal drop-in current' \
          'OK.*distro unit present'; do
  printf '%s\n' "$_o" | grep -q "$_w" || fail "check missed [$_w]: $_o"
done
printf '%s\n' "$_o" | grep -q 'user manager' \
  && fail "an unanswered manager query still said something"
printf '#!/bin/sh\nexit 3\n' > "$_xb"
_xp check | grep -q 'FAIL.*portal backend missing or does not run' \
  || fail "check accepted a backend that does not run"
printf '#!/bin/sh\nexit 0\n' > "$_xb"
printf 'stale\n' >> "$T/etc/u/x.service.d/50-wayfire-mods.conf"
_xp check | grep -q 'FAIL.*portal drop-in missing or stale' \
  || fail "check accepted a stale drop-in"
_xp xdpw-place > /dev/null
_o=$(SC_SAYS="{ path=/usr/libexec/xdg-desktop-portal-wlr ; }" _xp check || :)
printf '%s\n' "$_o" | grep -q 'WARN.*still starts the stock backend' \
  || fail "check missed a manager on the stock backend: $_o"
printf '%s\n' "$_o" | grep -q 'FAIL.*user manager' \
  && fail "an unreloaded manager was reported as a failure"
SC_SAYS="{ path=$_xb ; }" _xp check \
  | grep -q 'OK.*this user manager starts the patched backend' \
  || fail "check missed a manager on the patched backend"
pass
