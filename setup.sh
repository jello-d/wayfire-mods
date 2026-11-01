#!/bin/sh
# setup.sh - build / verify / check the wayfire-mods compositor stack: wlroots
# (clean) + wayfire (+ our patches) + wcm/plugins-extra/wf-info (clean) +
# filters/pixdecor (+ our patches) + our own plugins, built from source into a
# system prefix. The SINGLE entry point a consumer or provisioning layer uses.
#
#   install    build + install the stack into /opt/wayfire (root). INCREMENTAL:
#      a no-op when every component matches the current refs/patches/plugins.
#      Sudo-SAFE: if the prefix is not writable and no non-interactive sudo
#      exists, it SKIPS rather than hang (the real build runs where sudo is
#      authed: a provision's install phase, or a cutover).
#   verify     build into a THROWAWAY prefix, no sudo; proves the whole stack
#      still fetches, patches, and compiles against the pins without touching
#      /opt, then runs test/live.t on it headless so each patch is proven to
#      BEHAVE (CI / the check to run before bumping a ref).
#   check      wayfire installed, self-contained by rpath, the installed
#      build IS this checkout's (build id), key components present;
#      [OK]/[FAIL] markers.
#   uninstall  remove the prefix + the build stamps.
#   test       run the in-repo suite (test/run).
#   version    the packaged version + the upstream pins.
#
# POSIX sh. Unlike a ~/.local package this installs to a SYSTEM prefix, so it
# has its own WF_PREFIX (not the generic PREFIX a sweep passes). The heavy work
# is libexec/build-wayfire; this file owns the sudo gate, the verify sandbox,
# and the audit.
set -eu

PKG=wayfire-mods
VERSION=0.1.0
_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ -z "${HOME:-}" ]; then
  HOME=$(getent passwd "$(id -u)" 2>/dev/null | cut -d: -f6 || true)
  [ -n "$HOME" ] || { echo "$PKG: HOME unset" >&2; exit 1; }
  export HOME
fi

# Our OWN system prefix, deliberately NOT the generic ~/.local a pkg sweep
# passes: the compositor is a system component the greeter runs from here.
WF_PREFIX=${WF_PREFIX:-/opt/wayfire}
ARCH=$(cc -dumpmachine 2>/dev/null || gcc -dumpmachine 2>/dev/null \
       || echo x86_64-linux-gnu)
LIBDIR=$WF_PREFIX/lib/$ARCH
CACHE=${WBM_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/wayfire-mods}
WAYFIRE=$WF_PREFIX/bin/wayfire
RC=0

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  _G=$(printf '\033[32m'); _R=$(printf '\033[31m'); _O=$(printf '\033[0m')
else _G=; _R=; _O=; fi
ok()  { printf '  %s[OK]%s   %s\n' "$_G" "$_O" "$1"; }
bad() { printf '  %s[FAIL]%s %s\n' "$_R" "$_O" "$1"; RC=1; }

_safe_path() {   # <path> <verb>: refuse an empty, relative or shallow path
  case $1 in
  /*/*) ;;
  *) echo "$PKG: $2: refusing to remove '$1'" >&2; return 1 ;;
  esac
}

_build() {   # <prefix>: run the recipe against a prefix
  # env (not a bare assignment prefix): an assignment word produced by
  # expansion, such as ${WBM_NOSTAMP:+...}, is NOT recognised as one by the
  # shell and would be run as a command; env parses VAR=val args correctly.
  env WBM_ROOT="$_root" WBM_CACHE="$CACHE" ${WBM_NOSTAMP:+WBM_NOSTAMP=1} \
    sh "$_root/libexec/build-wayfire" "$1"
}

do_install() {
  # Sudo-SAFE: if the system prefix is not writable and there is no cached,
  # non-interactive sudo (e.g. a Link-phase staged run with no tty), skip rather
  # than hang on a password prompt. The real build runs where sudo is authed.
  if [ ! -w "$WF_PREFIX" ] && ! sudo -n true 2>/dev/null; then
    echo "$PKG: $WF_PREFIX needs sudo and none is available; skipping." >&2
    echo "$PKG:   (runs in a provision's install phase, or a sudo cutover)" >&2
    return 0
  fi
  _build "$WF_PREFIX"
}

do_verify() {
  _tmp=$(mktemp -d)
  # CHECKED, THEN BAKED IN AS A LITERAL: `rm -rf` never runs on a variable
  # expansion, and a single-quoted trap would defer it to fire time.
  _safe_path "$_tmp" verify || exit 1
  _v_c="'$_tmp'"
  # shellcheck disable=SC2064  # expanding NOW is the point; see above
  trap "rm -rf -- $_v_c" EXIT INT TERM
  echo "$PKG: verify build into $_tmp (throwaway, no sudo) ..."
  WBM_NOSTAMP=1 _build "$_tmp"
  [ -x "$_tmp/bin/wayfire" ] \
    || { echo "$PKG: verify FAILED (no wayfire produced)" >&2; exit 1; }
  # Then RUN it: test/live.t drives the throwaway prefix headless and proves
  # each patch still behaves, which applying cleanly does not. It skips
  # (exit 0, says why) where foot, grim or GTK3 python is missing, so the
  # verdict names which of the two was proven.
  _live=$(WFM_LIVE_PREFIX=$_tmp sh "$_root/test/live.t" 2>&1) || {
    printf '%s\n' "$_live" >&2
    echo "$PKG: verify FAILED (the stack builds, a patch misbehaves)" >&2
    exit 1; }
  printf '%s\n' "$_live"
  case $_live in
    *skip\ live*) echo "$PKG: verify OK (builds; live checks SKIPPED)" ;;
    *) echo "$PKG: verify OK (builds, and every patch behaves live)" ;;
  esac
}

do_uninstall() {
  # Both targets come from overridable variables (WF_PREFIX, WBM_CACHE), so
  # each is checked before anything is removed.
  _safe_path "$WF_PREFIX" uninstall || exit 1
  _safe_path "$CACHE" uninstall || exit 1
  if [ -d "$WF_PREFIX" ] && { [ -w "$WF_PREFIX" ] || sudo -n true 2>/dev/null; }
  then
    { [ -w "$WF_PREFIX" ] && rm -rf -- "$WF_PREFIX"; } \
      || sudo rm -rf -- "$WF_PREFIX"
  fi
  rm -rf -- "$CACHE/stamps"
  echo "$PKG: removed $WF_PREFIX + build stamps"
}

# A wayfire being installed is not this checkout being installed: a
# consumer that reinstalls only when its clone moves would otherwise keep a
# stale or half-built prefix forever, with this check green.
check_build_id() {
  _want=$(env WBM_ROOT="$_root" sh "$_root/libexec/build-wayfire" --build-id)
  _idf=$WF_PREFIX/share/wayfire-mods/build-id
  _have=$(cat "$_idf" 2>/dev/null || true)
  if [ -z "$_have" ]; then
    bad "no build id at $_idf (an older recipe, or an install that never \
finished): run setup.sh install"
  elif [ "$_have" = "$_want" ]; then
    ok "installed build is this checkout's (build id $_want)"
  else
    bad "installed build is NOT this checkout's (installed $_have, this \
checkout $_want): run setup.sh install"
  fi
}

do_check() {
  echo "== $PKG (compositor stack from source) =="
  if [ -x "$WAYFIRE" ]; then ok "wayfire installed ($WAYFIRE)"
  else bad "wayfire not installed ($WAYFIRE; run setup.sh install)"; return; fi
  if readelf -d "$WAYFIRE" 2>/dev/null | grep -E 'RUNPATH|RPATH' \
       | grep -qF "$LIBDIR"; then
    ok "self-contained by rpath ($LIBDIR)"
  else
    bad "wayfire has no rpath into $LIBDIR (rebuild: setup.sh install)"
  fi
  check_build_id
  # key components: our custom plugin + the patched satellites + the tools.
  for _c in "$LIBDIR/wayfire/libedge-flipping.so:custom plugins" \
            "$LIBDIR/wayfire/libpixdecor.so:pixdecor" \
            "$LIBDIR/wayfire/libfilters.so:filters" \
            "$WF_PREFIX/bin/wcm:wcm" "$WF_PREFIX/bin/wf-info:wf-info"; do
    _p=${_c%%:*}; _l=${_c#*:}
    [ -e "$_p" ] && ok "$_l present" || bad "$_l missing ($_p)"
  done
}

_U="usage: setup.sh [install|verify|check|uninstall|test|version]"
case "${1:-install}" in
  install)   do_install ;;
  verify)    do_verify ;;
  uninstall) do_uninstall ;;
  check)     do_check; exit "$RC" ;;
  test)      exec sh "$_root/test/run" ;;
  version)   . "$_root/refs"; echo \
    "$PKG $VERSION (wlroots $WLROOTS_REF, wayfire $WAYFIRE_REF)" ;;
  -h|--help|help) echo "$_U" ;;
  *) echo "setup.sh: unknown command '${1:-}'" >&2; echo "$_U" >&2; exit 2 ;;
esac
