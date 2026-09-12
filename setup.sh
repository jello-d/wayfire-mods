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
#      authed -- a provision's install phase, or a cutover).
#   verify     build into a THROWAWAY prefix, no sudo; proves the whole stack
#      still fetches, patches, and compiles against the pins without touching
#      /opt (CI / the check to run before bumping a ref).
#   check      wayfire installed, self-contained by rpath, key components
#      present; [OK]/[FAIL] markers.
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

_build() {   # <prefix>  -- run the recipe against a prefix
  # env (not a bare assignment prefix): an assignment word produced by expansion
  # -- ${WBM_NOSTAMP:+...} -- is NOT recognised as one by the shell and would be
  # run as a command; env parses its VAR=val args correctly.
  env WBM_ROOT="$_root" WBM_CACHE="$CACHE" ${WBM_NOSTAMP:+WBM_NOSTAMP=1} \
    sh "$_root/libexec/build-wayfire" "$1"
}

do_install() {
  # Sudo-SAFE: if the system prefix is not writable and there is no cached,
  # non-interactive sudo (e.g. a Link-phase staged run with no tty), skip rather
  # than hang on a password prompt. The real build runs where sudo is authed.
  if [ ! -w "$WF_PREFIX" ] && ! sudo -n true 2>/dev/null; then
    echo "$PKG: $WF_PREFIX needs sudo and none is available -- skipping." >&2
    echo "$PKG:   (runs in a provision's install phase, or a sudo cutover)" >&2
    return 0
  fi
  _build "$WF_PREFIX"
}

do_verify() {
  _tmp=$(mktemp -d)
  trap 'rm -rf "$_tmp"' EXIT INT TERM
  echo "$PKG: verify build into $_tmp (throwaway, no sudo) ..."
  WBM_NOSTAMP=1 _build "$_tmp"
  [ -x "$_tmp/bin/wayfire" ] \
    || { echo "$PKG: verify FAILED (no wayfire produced)" >&2; exit 1; }
  echo "$PKG: verify OK (whole stack builds against the pins)"
}

do_uninstall() {
  if [ -d "$WF_PREFIX" ] && { [ -w "$WF_PREFIX" ] || sudo -n true 2>/dev/null; }
  then
    { [ -w "$WF_PREFIX" ] && rm -rf "$WF_PREFIX"; } || sudo rm -rf "$WF_PREFIX"
  fi
  rm -rf "$CACHE/stamps"
  echo "$PKG: removed $WF_PREFIX + build stamps"
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
