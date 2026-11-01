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

WF_PID=; FOOT_PID=; DLG_PID=; SC_PID=; ENC_PID=; FAILS=0
# The test allowlist, in C sort order: what a sandboxed client must see,
# exactly. foot needs subcompositor and the data device to map at all, and
# with no decoration protocol listed it must draw its own decorations.
ALLOW="wl_compositor wl_data_device_manager wl_output wl_seat wl_shm
  wl_subcompositor xdg_wm_base"
ALLOW=$(printf '%s\n' $ALLOW | tr '\n' ' ' | sed 's/ $//')
MARK="255 0 255"
PLUGINS="ipc ipc-rules pixdecor filters security-context-v1 enclave-mark"

_live_stop() {
  for _p in $ENC_PID $SC_PID $DLG_PID $FOOT_PID $WF_PID; do
    kill "$_p" 2>/dev/null || :
  done
  ENC_PID=; SC_PID=; DLG_PID=; FOOT_PID=; WF_PID=
}
# Re-armed with the client cleanup in front; T was checked by harness_init
# and is baked in as a literal, as there.
_lc="'$T'"
# shellcheck disable=SC2064  # expanding NOW is the point
trap "_live_stop; rm -rf -- $_lc" EXIT INT TERM

check() {   # <name> <0 = pass> <detail>
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

write_fixtures() {
  mkdir -m 0700 "$T/data"
  build_secctx
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
  place "$_eid" 0 0 1280 720 true
  sleep 1
  _fs=$(at 2 360)
  if [ "$_em" = "$MARK" ] && [ "$_pm" != "$MARK" ] && [ "$_fs" = "$MARK" ]
  then check enclave-mark 0 "framed [$_em], personal [$_pm], fullscreen [$_fs]"
  else check enclave-mark 1 \
    "enclave [$_em] personal [$_pm] fullscreen [$_fs] (mark is [$MARK]);
    $(where "$_eid"); $(where "$_pid"); outputs $(jpy "[(o['name'],
    o['geometry']) for o in d]" < "$S/outputs.json")"
  fi
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
  else
    check pixdecor 1 "foot never mapped"; check filters 1 "foot never mapped"
    check enclave-mark 1 "foot never mapped"
  fi
  client dialog python3 -I "$HERE/test/live-dialog"; DLG_PID=$!
  check_ipc_guard
  _live_stop
}

write_fixtures
session_scale
session_main
[ "$FAILS" = 0 ] || fail "$FAILS live check(s) failed against $P"
pass "7 checks against $P"
