#!/bin/sh
# live.t - run a BUILT stack headless and prove each of our patches still
# does its job, by behaviour rather than by applying cleanly. Skips unless
# WFM_LIVE_PREFIX names a built prefix (setup.sh verify discards its own),
# so the fast suite stays fast:
#
#   WFM_LIVE_PREFIX=/var/tmp/wfm-prefix sh test/live.t
#
# Every check is exact, and each was proven to FAIL against a stock build
# of the same pins (every patch swapped for a no-op):
#
#   startup-image   both headless outputs log a committed startup frame
#   scale-clamp     a scale 0.5 1280x720 output stays 1280 wide, AND survives
#                   a window mapping on it (stock aborts: it hands 0.5 to
#                   wlroots' int preferred_buffer_scale, which asserts > 0)
#   pixdecor        title_font "sans 24" is 32px, so the bar is 48, not 56
#   filters         a shader reading `frame_margins` sees the titlebar
#   ipc-guard       moving a dialog to another output is refused over IPC
#                   and the compositor survives (stock aborts on a dassert)
#   security-ctx    a client through a security-context socket sees EXACTLY
#                   allowed_protocols, while a plain client still sees
#                   screencopy (stock has no allowlist: its denylist leaks
#                   everything it does not name)
#   enclave-mark    a window from that context, drawing its OWN decorations,
#                   carries the configured frame just outside its edge; a
#                   personal window does not; fullscreen moves the frame onto
#                   the inner edge (stock has no such plugin)
#   toplevel-capture a sandboxed client's window list holds ONLY its own
#                   instance's windows while a plain client's holds all; it
#                   captures its own window's real pixels, cannot even name a
#                   personal one, and keeps streaming a window that is
#                   entirely off-screen (stock has no such plugin)
#   capture-gpu     on a real GPU (a session of its own, only when a render
#                   node is usable and libgbm is present; otherwise a loud
#                   skip line): a window captures into a dmabuf of a format
#                   and modifier the session offered, AND into shm, both
#                   with the window's real pixels (stock: no such plugin)
#   activation      xdg-activation across sandbox boundaries, seven cases:
#                   a token from an enclave activates only its own enclave's
#                   windows; a personal token activates anything; a token
#                   with no creating surface cannot pull focus INTO an enclave
#                   (stock honours all seven)
#
# Two compositor runs, so the scale-0.5 crash on stock cannot hide the other
# checks. All checks run and report; the test fails if any did. A caller's
# WAYFIRE_PLUGIN_PATH passes through (it is searched first), which is how a
# single stock plugin is swapped in as a control; XDG_DATA_HOME is pinned to
# scratch so no user-installed plugin can leak in.
. "$(dirname "$0")/harness_lib"
harness_init live
P=${WFM_LIVE_PREFIX:-}
[ -n "$P" ] || skip "set WFM_LIVE_PREFIX to a built prefix"
[ -x "$P/bin/wayfire" ] || fail "no wayfire under $P"
for _c in foot grim python3 wayland-info wayland-scanner cc pkg-config; do
  command -v "$_c" >/dev/null 2>&1 || skip "needs $_c"
done
python3 -c 'import gi; gi.require_version("Gtk", "3.0")' 2>/dev/null \
  || skip "needs the GTK3 python bindings"

WF_PID=; FOOT_PID=; DLG_PID=; SC_PID=; ENC_PID=; TICK_PID=; FAILS=0
ACT_PIDS=
# The test allowlist, in C sort order: what a sandboxed client must see,
# exactly. foot needs subcompositor and the data device to map at all, and
# with no decoration protocol listed it must draw its own decorations.
# The three ext_ protocols are the window-sharing set (SV9).
ALLOW="ext_foreign_toplevel_image_capture_source_manager_v1
  ext_foreign_toplevel_list_v1 ext_image_copy_capture_manager_v1
  wl_compositor wl_data_device_manager wl_output wl_seat wl_shm
  wl_subcompositor xdg_activation_v1 xdg_wm_base"
ALLOW=$(printf '%s\n' $ALLOW | tr '\n' ' ' | sed 's/ $//')
MARK="255 0 255"
PLUGINS="ipc ipc-rules pixdecor filters security-context-v1 enclave-mark
  toplevel-capture xdg-activation"
PLUGINS=$(printf '%s ' $PLUGINS | sed 's/ $//')

_live_stop() {
  for _p in $ACT_PIDS $TICK_PID $ENC_PID $SC_PID $DLG_PID $FOOT_PID \
    $WF_PID; do
    kill "$_p" 2>/dev/null || :
  done
  ACT_PIDS=; TICK_PID=; ENC_PID=; SC_PID=; DLG_PID=; FOOT_PID=; WF_PID=
}
# Re-armed with the client cleanup in front; T was checked by harness_init
# and is baked in as a literal, as there.
_lc="'$T'"
# WFM_LIVE_KEEP=<dir>: copy the scratch tree (logs, client output) there
# before it is removed, to debug a failure after the fact.
_live_keep() {
  [ -n "${WFM_LIVE_KEEP:-}" ] || return 0
  mkdir -p "$WFM_LIVE_KEEP" && cp -r "$T/." "$WFM_LIVE_KEEP/" 2>/dev/null
  printf 'live: scratch tree kept in %s\n' "$WFM_LIVE_KEEP" >&2
}
# shellcheck disable=SC2064  # expanding NOW is the point
trap "_live_stop; _live_keep; rm -rf -- $_lc" EXIT INT TERM

CHECKS=0
check() {   # <name> <0 = pass> <detail>
  CHECKS=$((CHECKS + 1))
  if [ "$2" = 0 ]; then printf '  ok   %s: %s\n' "$1" "$3"
  else printf '  FAIL %s: %s\n' "$1" "$3"; FAILS=$((FAILS + 1)); fi
}
ipc() { python3 -I "$HERE/test/live-ipc" "$SOCK" "$@"; }
# jpy <python expr over d>: evaluate against the JSON on stdin
jpy() { python3 -I -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
alive() { [ -n "$WF_PID" ] && kill -0 "$WF_PID" 2>/dev/null; }
# died <name>: report a check that cannot run, with the compositor's last
# word: a wlroots assert, wayfire's own dassert message, or a fatal signal.
died() {
  check "$1" 1 "compositor not running: $(grep -m 1 -e Assert \
    -e 'Cannot move' -e Fatal "$S/wf.log" 2>/dev/null \
    || echo 'no fatal line in its log')"
}

# build_secctx: compile test/live-secctx.c against the staging protocol
build_secctx() {
  _x=$(pkg-config --variable=pkgdatadir wayland-protocols)
  _x=$_x/staging/security-context/security-context-v1.xml
  [ -f "$_x" ] || skip "needs wayland-protocols with security-context-v1"
  wayland-scanner client-header "$_x" "$T/security-context-v1-client-protocol.h"
  wayland-scanner private-code "$_x" "$T/security-context-v1-protocol.c"
  # shellcheck disable=SC2046  # pkg-config output is a word list
  cc -o "$T/secctx" "$HERE/test/live-secctx.c" \
    "$T/security-context-v1-protocol.c" -I"$T" \
    $(pkg-config --cflags --libs wayland-client) \
    || fail "could not build the security-context test client"
}

# build_capture: compile test/live-capture.c against its three protocols
build_capture() {
  _d=$(pkg-config --variable=pkgdatadir wayland-protocols)/staging
  for _x in ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1 \
            ext-image-capture-source/ext-image-capture-source-v1 \
            ext-image-copy-capture/ext-image-copy-capture-v1; do
    [ -f "$_d/$_x.xml" ] || skip "needs wayland-protocols with ${_x#*/}"
    wayland-scanner client-header "$_d/$_x.xml" \
      "$T/${_x#*/}-client-protocol.h"
    wayland-scanner private-code "$_d/$_x.xml" "$T/${_x#*/}-protocol.c"
  done
  # shellcheck disable=SC2046  # pkg-config output is a word list
  cc -o "$T/capture" "$HERE/test/live-capture.c" \
    "$T/ext-foreign-toplevel-list-v1-protocol.c" \
    "$T/ext-image-capture-source-v1-protocol.c" \
    "$T/ext-image-copy-capture-v1-protocol.c" -I"$T" \
    $(pkg-config --cflags --libs wayland-client) \
    || fail "could not build the capture test client"
  build_capture_gbm "$_d"
}

# build_capture_gbm <staging dir>: the same client WITH_GBM, for the GPU
# session; absent (and that session skipped, loudly) without libgbm
build_capture_gbm() {
  pkg-config --exists gbm || return 0
  _x=$(pkg-config --variable=pkgdatadir wayland-protocols)
  _x=$_x/stable/linux-dmabuf/linux-dmabuf-v1.xml
  [ -f "$_x" ] || return 0
  wayland-scanner client-header "$_x" "$T/linux-dmabuf-v1-client-protocol.h"
  wayland-scanner private-code "$_x" "$T/linux-dmabuf-v1-protocol.c"
  # shellcheck disable=SC2046  # pkg-config output is a word list
  cc -DWITH_GBM -o "$T/capture-gbm" "$HERE/test/live-capture.c" \
    "$T/ext-foreign-toplevel-list-v1-protocol.c" \
    "$T/ext-image-capture-source-v1-protocol.c" \
    "$T/ext-image-copy-capture-v1-protocol.c" \
    "$T/linux-dmabuf-v1-protocol.c" -I"$T" \
    $(pkg-config --cflags --libs wayland-client gbm) \
    || fail "could not build the GPU capture test client"
}

# build_activate: test/live-activate.c, with the virtual-keyboard protocol
# vendored in test/protocols (wlroots ships it but installs no copy)
build_activate() {
  _d=$(pkg-config --variable=pkgdatadir wayland-protocols)
  for _x in "$_d/stable/xdg-shell/xdg-shell.xml" \
            "$_d/staging/xdg-activation/xdg-activation-v1.xml" \
            "$HERE/test/protocols/virtual-keyboard-unstable-v1.xml"; do
    [ -f "$_x" ] || skip "needs $_x"
    _n=$(basename "$_x" .xml)
    wayland-scanner client-header "$_x" "$T/$_n-client-protocol.h"
    wayland-scanner private-code "$_x" "$T/$_n-protocol.c"
  done
  # shellcheck disable=SC2046  # pkg-config output is a word list
  cc -o "$T/activate" "$HERE/test/live-activate.c" \
    "$T/xdg-shell-protocol.c" "$T/xdg-activation-v1-protocol.c" \
    "$T/virtual-keyboard-unstable-v1-protocol.c" -I"$T" \
    $(pkg-config --cflags --libs wayland-client) \
    || fail "could not build the activation test client"
}

write_fixtures() {
  mkdir -m 0700 "$T/data"
  build_secctx
  build_capture
  build_activate
  printf '[csd]\npreferred=server\n' > "$T/foot.ini"
  python3 -I -c 'import sys; sys.stdout.buffer.write(
    b"P6\n64 64\n255\n" + b"\xff\x00\x00" * 4096)' > "$T/start.ppm"
  cat > "$T/frame.glsl" <<'EOF'
#version 300 es
@builtin_ext@
@builtin@

precision mediump float;

uniform sampler2D in_tex;
uniform vec4 frame_margins;
out vec4 out_color;
in mediump vec2 uvpos;

void main()
{
    out_color = frame_margins.w > 0.5 ? vec4(1.0, 0.0, 0.0, 1.0)
                                      : vec4(0.0, 1.0, 0.0, 1.0);
}
EOF
}

# write_ini <file> <outputs body>: the shared core section plus outputs
write_ini() {
  { printf '%s\n' '[core]' \
      "plugins = $PLUGINS" \
      'preferred_decoration_mode = server' 'xwayland = false' '' \
      '[pixdecor]' 'title_font = sans 24' 'border_size = 4' '' \
      '[security-context-v1]' \
      "allowed_protocols = $(printf '%s' "$ALLOW" | sed 's/ /, /g')" '' \
      '[enclave-mark]' 'thickness = 6' 'colors = titan=1.0 0.0 1.0 1.0' ''
    printf '%s\n' "$2"; } > "$1"
}

start_wayfire() {   # <session dir> <ini> <output count>
  S=$1; SOCK=$S/ipc.sock; RUN=$S/run
  mkdir -p "$S"; mkdir -m 0700 "$RUN"
  env -u WAYLAND_DISPLAY -u DISPLAY -u WAYFIRE_SOCKET \
    XDG_RUNTIME_DIR="$RUN" XDG_DATA_HOME="$T/data" WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS="$3" WLR_LIBINPUT_NO_DEVICES=1 \
    _WAYFIRE_SOCKET="$SOCK" WAYFIRE_STARTUP_IMAGE="$T/start.ppm" \
    "$P/bin/wayfire" -c "$2" > "$S/wf.log" 2>&1 &
  WF_PID=$!
  _i=0
  until ipc window-rules/list-outputs > "$S/outputs.json" 2>/dev/null; do
    _i=$((_i + 1))
    [ "$_i" -lt 100 ] || { tail -20 "$S/wf.log" >&2
      fail "wayfire did not answer IPC within 10s"; }
    alive || { tail -20 "$S/wf.log" >&2; fail "wayfire exited at startup"; }
    sleep 0.1
  done
  WLD=
  for _s in "$RUN"/wayland-*; do
    [ -S "$_s" ] && { WLD=${_s##*/}; break; }
  done
  [ -n "$WLD" ] || fail "no wayland socket in $RUN"
}

# client <log name> <cmd...>: run a client on the session, in the
# background. The `&` is on the command itself, not on a call to this
# function, so $! afterwards is the client and not a wrapper subshell.
client() {
  _cl=$1; shift
  WAYLAND_DISPLAY=$WLD XDG_RUNTIME_DIR=$RUN GDK_BACKEND=wayland \
    "$@" > "$S/$_cl.log" 2>&1 &
}
start_foot() {
  client foot foot -c "$T/foot.ini" --app-id wfm-foot sleep 600; FOOT_PID=$!
}

# wait_view <python predicate over v>: print the first matching view as JSON
wait_view() {
  _i=0
  while [ "$_i" -lt 100 ] && alive; do
    _v=$(ipc window-rules/list-views 2>/dev/null | jpy "json.dumps(next(
      (v for v in d if v.get('mapped') and ($1)), None))" 2>/dev/null)
    case $_v in null | '') ;; *) printf '%s\n' "$_v"; return 0 ;; esac
    _i=$((_i + 1)); sleep 0.1
  done
  return 1
}

check_startup_image() {
  _n=$(grep -c 'startup_image: HEADLESS-[12] submit=true commit=true' \
    "$S/wf.log")
  [ "$_n" = 2 ]; check startup-image $? "$_n of 2 outputs committed the frame"
}

# Session 1: ONE output at scale 0.5, and a real window mapped on it.
session_scale() {
  write_ini "$T/scale.ini" "$(printf '%s\n' '[output:HEADLESS-1]' \
    'mode = 1280x720@60000' 'scale = 0.5')"
  start_wayfire "$T/s1" "$T/scale.ini" 1
  _w=$(jpy "[int(o['geometry']['width']) for o in d
    if o['name'] == 'HEADLESS-1'][0]" < "$S/outputs.json")
  start_foot
  if ! wait_view "v['app-id'] == 'wfm-foot'" > /dev/null; then
    if alive; then check scale-clamp 1 "foot never mapped"
    else died scale-clamp; fi
  else
    [ "$_w" = 1280 ]
    check scale-clamp $? "output is $_w wide, survived a window mapping"
  fi
  _live_stop
}

check_pixdecor() {   # <foot view json>
  # IPC geometry is float; title = top margin minus the (border) bottom one
  _t=$(printf '%s\n' "$1" | jpy "int((d['base-geometry']['y']
    - d['geometry']['y']) - ((d['geometry']['y'] + d['geometry']['height'])
    - (d['base-geometry']['y'] + d['base-geometry']['height'])))")
  [ "$_t" = 48 ]; check pixdecor $? "titlebar is $_t px (want 48)"
}

check_filters() {   # <foot view json>
  _id=$(printf '%s\n' "$1" | jpy "d['id']")
  _xy=$(printf '%s\n' "$1" | jpy "'%d,%d' % (
    d['base-geometry']['x'] + d['base-geometry']['width'] // 2,
    d['base-geometry']['y'] + d['base-geometry']['height'] // 2)")
  ipc wf/filters/set-view-shader \
    "{\"view-id\": $_id, \"shader-path\": \"$T/frame.glsl\"}" > /dev/null
  sleep 1
  _px=$(WAYLAND_DISPLAY=$WLD XDG_RUNTIME_DIR=$RUN grim -g "$_xy 1x1" \
    -t ppm - | python3 -I -c 'import sys; print(*sys.stdin.buffer.read()[-3:])')
  [ "$_px" = "255 0 0" ]
  check filters $? "client pixel under the shader is [$_px] (want 255 0 0)"
}

check_ipc_guard() {
  if ! _d=$(wait_view "v['title'] == 'wfm-dialog' and v['parent'] != -1")
  then
    if alive; then check ipc-guard 1 "the dialog never mapped with a parent"
    else died ipc-guard; fi
    return
  fi
  _id=$(printf '%s\n' "$_d" | jpy "d['id']")
  _on=$(printf '%s\n' "$_d" | jpy "d['output-id']")
  # The OTHER output: a move to the one it is already on returns early in
  # upstream before any parent check, which proves nothing either way.
  _o2=$(jpy "[o['id'] for o in d if o['id'] != $_on][0]" < "$S/outputs.json")
  _r=$(ipc window-rules/configure-view "{\"id\": $_id, \"output_id\": $_o2}")
  _rc=$?
  sleep 0.5
  if [ "$_rc" != 0 ] || ! alive; then
    died ipc-guard; return
  fi
  case $_r in
    *child/dialog*) check ipc-guard 0 "refused, compositor alive" ;;
    *) check ipc-guard 1 "unexpected reply: $_r" ;;
  esac
}

# interfaces <display>: the distinct global names a client on it is offered,
# sorted (unique: there is one wl_output global per output)
interfaces() {
  WAYLAND_DISPLAY=$1 XDG_RUNTIME_DIR=$RUN wayland-info 2>/dev/null \
    | sed -n "s/^interface: '\([^']*\)'.*/\1/p" | LC_ALL=C sort -u \
    | tr '\n' ' ' \
    | sed 's/ $//'
}

check_secctx() {
  client secctx "$T/secctx" "$S/ctx.sock" titan; SC_PID=$!
  _i=0
  until grep -q ready "$S/secctx.log" 2>/dev/null; do
    _i=$((_i + 1))
    if [ "$_i" -ge 50 ] || ! kill -0 "$SC_PID" 2>/dev/null; then
      check security-ctx 1 "no context: $(cat "$S/secctx.log")"; return
    fi
    sleep 0.1
  done
  _seen=$(interfaces "$S/ctx.sock")
  _plain=$(interfaces "$WLD")
  if [ "$_seen" != "$ALLOW" ]; then
    check security-ctx 1 "sandboxed client sees [$_seen], want [$ALLOW]"
  else
    case " $_plain " in
      *" zwlr_screencopy_manager_v1 "*)
        check security-ctx 0 "sandboxed sees exactly the allowlist" ;;
      *) check security-ctx 1 "a PLAIN client lost screencopy: [$_plain]" ;;
    esac
  fi
}

# pixel <x> <y>: the "r g b" of one pixel of the session, layout coords
pixel() {
  WAYLAND_DISPLAY=$WLD XDG_RUNTIME_DIR=$RUN grim -g "$1,$2 1x1" -t ppm - \
    | python3 -I -c 'import sys; print(*sys.stdin.buffer.read()[-3:])'
}

# place <view id> <x> <y> <w> <h> [fullscreen]: onto HEADLESS-1, in its
# local coordinates. Do not assume where it sits in the layout (measured: at
# x=1280, after HEADLESS-2): `at` turns local coordinates into layout ones.
place() {
  _o1=$(jpy "[o['id'] for o in d if o['name'] == 'HEADLESS-1'][0]" \
    < "$S/outputs.json")
  ipc window-rules/configure-view "{\"id\": $1, \"output_id\": $_o1,
    \"fullscreen\": ${6:-false}, \"geometry\": {\"x\": $2, \"y\": $3,
    \"width\": $4, \"height\": $5}}" > /dev/null
}

# where <view id>: its output and geometry, for a failure message
where() {
  ipc window-rules/view-info "{\"id\": $1}" | jpy "'view %d on %s at %s' % (
    d['info']['id'], d['info']['output-name'], d['info']['geometry'])"
}

at() {   # <local x> <local y>: the HEADLESS-1 pixel there, layout coords
  _ox=$(jpy "[int(o['geometry']['x']) for o in d
    if o['name'] == 'HEADLESS-1'][0]" < "$S/outputs.json")
  _oy=$(jpy "[int(o['geometry']['y']) for o in d
    if o['name'] == 'HEADLESS-1'][0]" < "$S/outputs.json")
  pixel $((_ox + $1)) $((_oy + $2))
}

# tab_stats <view id>: "<mark> <ink>" pixel counts in the strip just above
# the view's frame at its right end, where the name tab sits: <mark> in the
# enclave colour, <ink> in neither that nor the background (the lettering).
tab_stats() {
  _g=$(ipc window-rules/view-info "{\"id\": $1}" | jpy "'%d %d %d' % (
    d['info']['geometry']['x'], d['info']['geometry']['y'],
    d['info']['geometry']['width'])")
  set -- $_g
  _ox=$(jpy "[int(o['geometry']['x']) for o in d
    if o['name'] == 'HEADLESS-1'][0]" < "$S/outputs.json")
  # thickness 6: the frame's outer right edge is x+w+6, its top is y-6
  WAYLAND_DISPLAY=$WLD XDG_RUNTIME_DIR=$RUN grim -g \
    "$((_ox + $1 + $3 + 6 - 40)),$(($2 - 6 - 12)) 40x12" -t ppm - \
    | python3 -I -c 'import sys
d = sys.stdin.buffer.read()
px = d[d.index(b"255\n") + 4:]
mark = ink = 0
for i in range(0, len(px), 3):
    p = tuple(px[i:i + 3])
    if p == (255, 0, 255): mark += 1
    elif p != (25, 25, 25): ink += 1
print(mark, ink)'
}

check_enclave_mark() {   # <personal foot view json>
  WAYLAND_DISPLAY=$S/ctx.sock XDG_RUNTIME_DIR=$RUN foot -c "$T/foot.ini" \
    --app-id wfm-enclave sleep 600 > "$S/enclave.log" 2>&1 &
  ENC_PID=$!
  if ! _e=$(wait_view "v['app-id'] == 'wfm-enclave'"); then
    check enclave-mark 1 "the enclave foot never mapped: $(tail -n 1 \
      "$S/enclave.log")"; return
  fi
  _eid=$(printf '%s\n' "$_e" | jpy "d['id']")
  _pid=$(printf '%s\n' "$1" | jpy "d['id']")
  place "$_eid" 100 100 400 300; place "$_pid" 700 100 400 300
  sleep 1
  _em=$(at 97 250); _pm=$(at 697 250)
  _et=$(tab_stats "$_eid"); _pt=$(tab_stats "$_pid")
  place "$_eid" 0 0 1280 720 true
  sleep 1
  _fs=$(at 2 360)
  # The name tab: mostly the enclave colour with lettering on it, and
  # nothing of the kind above a personal window.
  _tab=1
  # shellcheck disable=SC2086  # "<mark> <ink>", two numbers
  set -- $_et $_pt
  [ "$1" -gt 150 ] && [ "$2" -gt 5 ] && [ "$3" = 0 ] && _tab=0
  if [ "$_em" = "$MARK" ] && [ "$_pm" != "$MARK" ] && [ "$_fs" = "$MARK" ] \
    && [ "$_tab" = 0 ]
  then check enclave-mark 0 \
    "framed [$_em], personal [$_pm], fullscreen [$_fs], tab [$_et]"
  else check enclave-mark 1 \
    "enclave [$_em] personal [$_pm] fullscreen [$_fs] tab [$_et] personal-tab
    [$_pt] (mark is [$MARK]);
    $(where "$_eid"); $(where "$_pid"); outputs $(jpy "[(o['name'],
    o['geometry']) for o in d]" < "$S/outputs.json")"
  fi
}

# capture_as <display> <client args...> [/path/to/client]: run the capture
# client on that display; a trailing absolute path picks another build (the
# GPU session's gbm one). `list` has one argument, `stream` three.
capture_as() {
  _cd=$1; shift
  _cb=$T/capture
  if [ "$#" = 3 ]; then
    case $3 in /*) _cb=$3; set -- "$1" "$2" ;; esac
  fi
  WAYLAND_DISPLAY=$_cd XDG_RUNTIME_DIR=$RUN timeout 10 "$_cb" "$@" 2>/dev/null
}

has() { case $1 in *"$2"*) return 0 ;; esac; return 1; }   # <text> <part>

check_toplevel_capture() {
  WAYLAND_DISPLAY=$S/ctx.sock XDG_RUNTIME_DIR=$RUN foot --app-id wfm-tick \
    -o colors.background=00ff00 sh -c \
    'while :; do printf "\033[2J%s" "$(date +%N)"; sleep 0.2; done' \
    > "$S/tick.log" 2>&1 &
  TICK_PID=$!
  _t=$(wait_view "v['app-id'] == 'wfm-tick'") || {
    check toplevel-capture 1 "the ticker never mapped"; return; }
  _mine=$(capture_as "$S/ctx.sock" list | tr '\n' ' ')
  _all=$(capture_as "$WLD" list | tr '\n' ' ')
  _own=$(capture_as "$S/ctx.sock" capture wfm-tick)
  _foreign=$(capture_as "$S/ctx.sock" capture wfm-foot)
  place "$(printf '%s\n' "$_t" | jpy "d['id']")" 5000 5000 400 300
  sleep 0.5
  _stream=$(capture_as "$S/ctx.sock" stream wfm-tick 3)
  # Resized while streaming (about 0.5s into ~2s of frames): exactly one
  # frame must be redone at the new size, and the stream must carry on.
  ( sleep 0.5; place "$(printf '%s\n' "$_t" | jpy "d['id']")" 5000 5000 \
    500 400 ) &
  _rz=$!
  _resize=$(capture_as "$S/ctx.sock" stream wfm-tick 10)
  # Only the resizer: a bare `wait` also waits for wayfire and every client.
  wait "$_rz"
  _detail="sandbox lists [$_mine]; own [$_own]; foreign [$_foreign];"
  _detail="$_detail off-screen [$_stream]; resized mid-stream [$_resize]"
  _ok=0
  has "$_mine" "toplevel wfm-foot" && _ok=1        # a personal window leaked
  has "$_mine" "toplevel wfm-tick" || _ok=1        # its own window missing
  has "$_mine" "lists 1" || _ok=1                  # exactly one list
  has "$_all" "toplevel wfm-foot" || _ok=1         # personal sees everything
  has "$_all" "toplevel wfm-tick" || _ok=1
  case $_own in "captured "*" 0 255 0") ;; *) _ok=1 ;; esac
  [ "$_foreign" = "not found" ] || _ok=1
  # retries 0: a session opened AFTER the move must be told the new size
  [ "$_stream" = "streamed 3 retries 0" ] || _ok=1
  [ "$_resize" = "streamed 10 retries 1" ] || _ok=1
  check toplevel-capture "$_ok" "$_detail"
}

# act_display <personal|titan|work>: the socket a client of that side uses
act_display() {
  case $1 in personal) echo "$WLD" ;; titan) echo "$S/ctx.sock" ;;
    *) echo "$S/ctx-$1.sock" ;; esac
}

# until_true <shell condition>: poll it for up to 5s; returns its last status
until_true() {
  _u=0
  until eval "$1"; do
    _u=$((_u + 1)); [ "$_u" -lt 50 ] || return 1; sleep 0.1
  done
}
focused_app() {
  ipc window-rules/get-focused-view | jpy "d['info']['app-id']"
}

# act_case <creator side> <target side> <surface|nosurface>: one activation.
# Appends to $_got: "a" if the target took focus (honoured), "c" if the
# creator kept it, "x" no token, "?" no request. Runs in THIS shell, never in
# $(...): a subshell would lose the case counter and the recorded PIDs.
act_case() {
  _n=$((_n + 1)); _ci=$S/c$_n.in; _ai=$S/a$_n.in
  mkfifo "$_ci" "$_ai"
  WAYLAND_DISPLAY=$(act_display "$1") XDG_RUNTIME_DIR=$RUN "$T/activate" \
    token "c$_n" < "$_ci" > "$S/c$_n.out" 2> "$S/c$_n.err" &
  ACT_PIDS="$ACT_PIDS $!"
  exec 7> "$_ci"
  WAYLAND_DISPLAY=$(act_display "$2") XDG_RUNTIME_DIR=$RUN "$T/activate" \
    activate "a$_n" < "$_ai" > "$S/a$_n.out" 2>&1 &
  ACT_PIDS="$ACT_PIDS $!"
  exec 8> "$_ai"
  wait_view "v['app-id'] == 'a$_n'" > /dev/null
  _c=$(wait_view "v['app-id'] == 'c$_n'" | jpy "d['id']")
  ipc window-rules/focus-view "{\"id\": $_c}" > /dev/null
  # Wait on the CONDITIONS, never a fixed sleep: the session is busy (a
  # software renderer, earlier checks' clients), and a token minted before
  # focus lands is refused for a reason unrelated to this test.
  until_true "[ \"\$(focused_app)\" = c$_n ]"
  sleep 0.2   # the enter event, and its serial, reach the client
  echo "$3" >&7
  until_true "grep -q '^token' '$S/c$_n.out'"
  _tok=$(sed -n 's/^token //p' "$S/c$_n.out")
  printf '%s\n' "$_tok" >&8
  until_true "grep -q requested '$S/a$_n.out'"
  sleep 0.3   # the focus change, if any, is applied
  if [ -z "$_tok" ]; then _got=${_got}x
  elif ! grep -q requested "$S/a$_n.out"; then _got=$_got'?'
  else _got=$_got$(focused_app | cut -c1)
  fi
  exec 7>&- 8>&-
}

check_activation() {
  client actctx "$T/secctx" "$S/ctx-work.sock" work; ACT_PIDS=$!
  client kbd "$T/activate" kbd; ACT_PIDS="$ACT_PIDS $!"
  sleep 1
  _n=0; _got=
  for _c in "titan titan surface" "titan personal surface" \
            "titan work surface" "personal titan surface" \
            "personal personal surface" "personal titan nosurface" \
            "personal personal nosurface"; do
    # shellcheck disable=SC2086  # the case is three words on purpose
    act_case $_c
  done
  [ "$_got" = accaaca ]
  check activation $? "got [$_got], want [accaaca] (a: honoured, c: denied)"
}

# Session 2: two scale-1 outputs, for everything else.
session_main() {
  write_ini "$T/main.ini" "$(printf '%s\n' '[output:HEADLESS-1]' \
    'mode = 1280x720@60000' '' '[output:HEADLESS-2]' 'mode = 1280x720@60000')"
  start_wayfire "$T/s2" "$T/main.ini" 2
  check_startup_image
  check_secctx
  start_foot
  if _foot=$(wait_view "v['app-id'] == 'wfm-foot'"); then
    check_pixdecor "$_foot"
    check_filters "$_foot"
    check_enclave_mark "$_foot"
    check_toplevel_capture
    check_activation
  else
    check pixdecor 1 "foot never mapped"; check filters 1 "foot never mapped"
    check enclave-mark 1 "foot never mapped"
    check toplevel-capture 1 "foot never mapped"
    check activation 1 "foot never mapped"
  fi
  client dialog python3 -I "$HERE/test/live-dialog"; DLG_PID=$!
  check_ipc_guard
  _live_stop
}

# Session 3: the same capture, on a real GPU, so a dmabuf path exists.
session_gpu() {
  _rn=
  for _n in /dev/dri/renderD*; do
    [ -r "$_n" ] && [ -w "$_n" ] && { _rn=$_n; break; }
  done
  if [ -z "$_rn" ] || [ ! -x "$T/capture-gbm" ]; then
    _why=${_rn:+no libgbm}
    printf '  skip capture-gpu: %s\n' "${_why:-no usable render node}"
    return
  fi
  write_ini "$T/gpu.ini" "$(printf '%s\n' '[output:HEADLESS-1]' \
    'mode = 1280x720@60000')"
  WLR_RENDER_DRM_DEVICE=$_rn start_wayfire "$T/s3" "$T/gpu.ini" 1
  client gpu-win foot -o colors.background=00ff00 --app-id wfm-gpu sleep 600
  FOOT_PID=$!
  wait_view "v['app-id'] == 'wfm-gpu'" > /dev/null \
    || { check capture-gpu 1 "the window never mapped"; _live_stop; return; }
  _dma=$(capture_as "$WLD" capture-dmabuf wfm-gpu "$T/capture-gbm")
  _shm=$(capture_as "$WLD" capture wfm-gpu "$T/capture-gbm")
  _ok=1
  case "$_dma|$_shm" in
    "captured-dmabuf "*" 0 255 0 mod "*"|captured "*" 0 255 0") _ok=0 ;;
  esac
  check capture-gpu "$_ok" "on $_rn: dmabuf [$_dma]; shm [$_shm]"
  _live_stop
}

write_fixtures
session_scale
session_main
session_gpu
[ "$FAILS" = 0 ] || fail "$FAILS live check(s) failed against $P"
pass "$CHECKS checks against $P"
