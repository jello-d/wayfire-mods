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
#   ipc-context     the IPC view description carries the client's security
#                   context: absent for a personal window, the engine /
#                   app-id / instance for an enclave one, and present but
#                   EMPTY for a context that named nothing (three states)
#   toplevel-capture a sandboxed client's window list holds ONLY its own
#                   instance's windows while a plain client's holds all; it
#                   captures its own window's real pixels, cannot even name a
#                   personal one, and keeps streaming a window that is
#                   entirely off-screen (stock has no such plugin)
#   xdpw-window-only the patched portal-wlr, on a private bus with a private
#                   pipewire: through the SANDBOX socket it starts, offers
#                   windows only (AvailableSourceTypes 2), shows its chooser
#                   exactly the sandbox's own windows and no monitor, starts
#                   a stream of one that a browser-shaped consumer READS
#                   frames from, restores that window for a second session
#                   without re-running the chooser, and refuses a
#                   monitor-only request;
#                   through the plain socket it offers both (3). Stock exits
#                   on the sandbox socket ("supports neither"). Skipped with
#                   a loud line when pipewire, dbus-run-session or the
#                   built backend is absent.
#   ipc-identifier  the IPC view description carries the window's
#                   ext-foreign-toplevel-list identifiers, each EQUAL to what
#                   a client of that list was sent: "toplevel-identifier" to
#                   a plain client's, security-context's own to the enclave
#                   client's, the two distinct, and already present in the
#                   view-mapped event (stock: no such keys)
#   mark-hugs       an enclave window that pixdecor decorates, with shadows and
#                   rounded corners on (the user's real setup): the frame sits
#                   on the VISIBLE edge, not a shadow-width out where the
#                   shadow-inclusive geometry would put it, and its corners
#                   are rounded like the window's (session 4)
#   capture-gpu     on a real GPU (a session of its own, only when a render
#                   node is usable and libgbm is present; otherwise a loud
#                   skip line): a window captures into a dmabuf of a format
#                   and modifier the session offered, AND into shm, both
#                   with the window's real pixels (stock: no such plugin)
#   edge-flip-armed (see edge-warp-off)
#   edge-warp       pushed across the part of an edge with nothing beyond
#                   it, the pointer lands on the neighbouring output (stock
#                   has no such plugin; it stops dead)
#   edge-warp-off   switched OFF live over IPC, that part is a wall again,
#                   AND edge-flipping does not flip there: an edge any output
#                   touches is interior to it, which is the one thing the two
#                   plugins' independence rests on. ARMED first: a push into
#                   a truly exterior edge must flip, or "no flip" proves
#                   nothing (pointer flipping defaults OFF, and the first
#                   version of this check passed against a planted
#                   edge-flipping for exactly that reason)
#   edge-warp-prop  proportional, set live, lands at the same fraction along
#   backoff-alive   an output whose every frame fails its test (a shim
#                   preloaded into this run, test/live-failtest.c, as when a
#                   dock's connector vanishes) under a window that keeps
#                   redrawing AND a running animation: IPC still answers
#                   (stock livelocks, repainting from an idle callback
#                   forever, so it never polls again)
#   backoff-rate    failures stay near one per 16ms, and are NOT zero (zero
#                   would mean the shim failed nothing and proved nothing)
#   backoff-recover once the output's tests pass again its frames commit:
#                   the failures stop and a screenshot of it succeeds
#   unplug-removed  the undock itself, nested: WL-2 of a wayfire running
#                   inside another fails every frame, then its window is
#                   closed from outside, which is wlroots' backend removing
#                   the output exactly as a hotplug does. With the fault
#                   STILL on, the nested wayfire drops WL-2 (stock never
#                   reads the removal: the same livelock, IPC dead)
#   unplug-quiet    and the failures stop, because the output is gone
#   ipc-name        a client that never reads its replies is dropped, and the
#                   log line names it: its pid, its process name and the
#                   reason (stock says only "Error sending json to client!",
#                   which left a dropped event stream unexplained for weeks)
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
CTX_PIDS=
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
  for _p in $CTX_PIDS $ACT_PIDS $TICK_PID $ENC_PID $SC_PID $DLG_PID \
    $FOOT_PID $WF_PID; do
    kill "$_p" 2>/dev/null || :
  done
  CTX_PIDS=; ACT_PIDS=; TICK_PID=; ENC_PID=; SC_PID=; DLG_PID=; FOOT_PID=
  WF_PID=
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

# build_failtest: test/live-failtest.c as a preload, against the wlroots
# headers of the prefix under test (it reads struct wlr_output's name)
build_failtest() {
  _a=$(cc -dumpmachine 2>/dev/null || echo x86_64-linux-gnu)
  # shellcheck disable=SC2046  # pkg-config output is a word list
  cc -shared -fPIC -DWLR_USE_UNSTABLE -o "$T/failtest.so" \
    "$HERE/test/live-failtest.c" \
    $(PKG_CONFIG_PATH="$P/lib/$_a/pkgconfig" pkg-config --cflags \
      wlroots-0.20) -ldl \
    || fail "could not build the failtest preload"
}

# build_pwconsume: the browser-shaped PipeWire consumer xdpw-window-only
# reads a started stream with; absent (that check skipped, loudly) without
# libpipewire's headers
build_pwconsume() {
  pkg-config --exists libpipewire-0.3 || return 0
  # shellcheck disable=SC2046  # pkg-config output is a word list
  cc -o "$T/pwconsume" "$HERE/test/live-pwconsume.c" \
    $(pkg-config --cflags --libs libpipewire-0.3) \
    || fail "could not build the pipewire consumer"
}

write_fixtures() {
  mkdir -m 0700 "$T/data"
  build_secctx
  build_capture
  build_pwconsume
  build_activate
  build_failtest
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
  # WF_ENV: extra VAR=value words for this compositor ONLY (e.g. a
  # preload); env parses them, none hold spaces.
  # shellcheck disable=SC2086  # WF_ENV is a word list
  env -u WAYLAND_DISPLAY -u DISPLAY -u WAYFIRE_SOCKET ${WF_ENV:-} \
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

# context_of <view id>: its IPC security-context, as "absent" or
# "engine|app-id|instance"
context_of() {
  ipc window-rules/view-info "{\"id\": $1}" | jpy "'absent' if
    'security-context' not in d['info'] else '|'.join((
    d['info']['security-context'].get('sandbox-engine', '?'),
    d['info']['security-context'].get('app-id', '?'),
    d['info']['security-context'].get('instance-id', '?')))"
}

check_ipc_context() {   # <personal foot view json>
  client anonctx "$T/secctx" "$S/ctx-anon.sock" -; CTX_PIDS=$!
  until_true "grep -q ready '$S/anonctx.log'"
  WAYLAND_DISPLAY=$S/ctx-anon.sock XDG_RUNTIME_DIR=$RUN foot \
    -c "$T/foot.ini" --app-id wfm-anon sleep 600 > "$S/anon.log" 2>&1 &
  CTX_PIDS="$CTX_PIDS $!"
  _a=$(wait_view "v['app-id'] == 'wfm-anon'" | jpy "d['id']")
  _e=$(wait_view "v['app-id'] == 'wfm-enclave'" | jpy "d['id']")
  _p=$(printf '%s\n' "$1" | jpy "d['id']")
  _cp=$(context_of "$_p"); _ce=$(context_of "$_e"); _ca=$(context_of "$_a")
  _d="personal [$_cp] enclave [$_ce] unnamed [$_ca]"
  if [ "$_cp" = absent ] && [ "$_ce" = "wayfire-mods-test|wfm-test|titan" ] \
     && [ "$_ca" = "||" ]; then
    check ipc-context 0 "$_d"
  else
    check ipc-context 1 "$_d"
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

# check_xdpw: the patched portal backend against both sockets (see the
# header). Every process is started by name and stopped by its own pid.
XDPW=${WFM_LIVE_XDPW:-$P/libexec/xdg-desktop-portal-wlr}
check_xdpw() {
  _why=
  for _c in pipewire dbus-run-session busctl; do
    command -v "$_c" > /dev/null 2>&1 || _why="needs $_c"
  done
  [ -x "$XDPW" ] || _why="no backend at $XDPW"
  [ -x "$T/pwconsume" ] || _why="no pipewire consumer (libpipewire-0.3-dev)"
  if [ -n "$_why" ]; then
    printf '  skip xdpw-window-only: %s\n' "$_why"; return
  fi
  XDG_RUNTIME_DIR=$RUN pipewire > "$S/pw.log" 2>&1 &
  _pw=$!
  _i=0
  until [ -S "$RUN/pipewire-0" ] || [ "$_i" -ge 50 ]; do
    _i=$((_i + 1)); sleep 0.1
  done
  _x() {   # <display> <dir>
    mkdir -p "$2"
    XDG_RUNTIME_DIR=$RUN timeout 60 dbus-run-session \
      --config-file="$HERE/test/live-xdpw-bus.conf" -- \
      sh "$HERE/test/live-xdpw" "$XDPW" "$1" "$2" "$T/pwconsume" \
      2> "$2/err" | tr '\n' ' '
  }
  _sb=$(_x "$S/ctx.sock" "$S/xdpw-sandbox")
  _pl=$(_x "$RUN/$WLD" "$S/xdpw-plain")
  kill "$_pw" 2>/dev/null; wait "$_pw" 2>/dev/null
  # The chooser's lines end "(<identifier>)"; the capture client lists
  # "toplevel <app-id> <identifier>". The two sets must be EQUAL, taken
  # straight after, so the count cannot drift between two looks.
  _own=$(capture_as "$S/ctx.sock" list | awk '$1 == "toplevel" {print $3}' \
    | sort | tr '\n' ' ')
  _got=$(sed -n 's/^Window: .*(\([0-9a-f]*\))$/\1/p' \
    "$S/xdpw-sandbox/offered" 2>/dev/null | sort | tr '\n' ' ')
  _mon=$(grep -c '^Monitor' "$S/xdpw-sandbox/offered" 2>/dev/null) || _mon=0
  _ok=0
  has "$_sb" "types 2 " || _ok=1          # windows only through the sandbox
  [ "$_got" = "$_own" ] || _ok=1          # exactly its own windows
  [ -n "$_own" ] || _ok=1                 # and it has some to offer
  [ "$_mon" = 0 ] || _ok=1                # never a monitor
  has "$_sb" "started 0 " || _ok=1        # Start created a stream
  has "$_sb" "streams 1 " || _ok=1
  # and something can READ it: Start succeeding is not a share working
  case $_sb in *"consumed "[1-9]*) ;; *) _ok=1 ;; esac
  has "$_sb" "monitor 0 " && _ok=1        # a monitor-only Start is refused
  has "$_sb" "monitor " || _ok=1
  # A second session handed the first one's restore data reuses the window
  # without asking (Chrome previews through one session and shares through
  # another; 0.8.1 restores only outputs, so the person picked twice).
  has "$_sb" "restore yes" || _ok=1
  has "$_sb" "reprompt 0 " || _ok=1
  case $_sb in *"restored consumed "[1-9]*) ;; *) _ok=1 ;; esac
  has "$_sb" "alive yes" || _ok=1         # and it survived all of it
  has "$_pl" "types 3 " || _ok=1          # the plain client keeps both
  case $_pl in *"consumed "[1-9]*) ;; *) _ok=1 ;; esac
  check xdpw-window-only "$_ok" "sandbox [$_sb] offered [$_got] own [$_own]\
 monitors $_mon; plain [$_pl]"
}

# ident_of <list output> <app-id>: the identifier that list gave the window
ident_of() {
  printf '%s\n' "$1" | awk -v a="$2" '$1 == "toplevel" && $2 == a { print $3 }'
}
# ipc_ids <view json>: "<toplevel-identifier>|<security-context's>"
ipc_ids() {
  printf '%s\n' "$1" | jpy "'%s|%s' % (d.get('toplevel-identifier', ''),
    d.get('security-context', {}).get('toplevel-identifier', ''))"
}

# check_ipc_identifier: after check_toplevel_capture, with its ticker (an
# enclave window) and the personal foot still mapped
check_ipc_identifier() {
  _mine=$(capture_as "$S/ctx.sock" list); _all=$(capture_as "$WLD" list)
  _ti=$(ident_of "$_mine" wfm-tick); _tp=$(ident_of "$_all" wfm-tick)
  _fp=$(ident_of "$_all" wfm-foot)
  _t=$(ipc_ids "$(wait_view "v['app-id'] == 'wfm-tick'")")
  _f=$(ipc_ids "$(wait_view "v['app-id'] == 'wfm-foot'")")
  python3 -I "$HERE/test/live-ipc" "$SOCK" --watch view-mapped \
    > "$S/watch.log" 2>&1 &
  CTX_PIDS="$CTX_PIDS $!"
  until_true "grep -q watching '$S/watch.log'"
  WAYLAND_DISPLAY=$S/ctx.sock XDG_RUNTIME_DIR=$RUN foot -c "$T/foot.ini" \
    --app-id wfm-idev sleep 600 > "$S/idev.log" 2>&1 &
  CTX_PIDS="$CTX_PIDS $!"
  until_true "grep -q '\"event\"' '$S/watch.log'"
  _ev=$(grep '"event"' "$S/watch.log" | jpy "d['view']['app-id']")
  _ev="$_ev $(ipc_ids "$(grep '"event"' "$S/watch.log" | jpy \
    "json.dumps(d['view'])")")"
  _d="enclave list [$_ti] plain list [$_tp] ipc [$_t];"
  _d="$_d personal plain [$_fp] ipc [$_f]; mapped event [$_ev]"
  _ok=0
  [ -n "$_ti" ] && [ -n "$_tp" ] && [ -n "$_fp" ] || _ok=1
  [ "$_ti" != "$_tp" ] || _ok=1           # each list mints its own
  [ "$_t" = "$_tp|$_ti" ] || _ok=1
  [ "$_f" = "$_fp|" ] || _ok=1             # personal: no instance identifier
  case $_ev in "wfm-idev "?*"|"?*) ;; *) _ok=1 ;; esac   # both, at map
  check ipc-identifier "$_ok" "$_d"
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
    check_ipc_context "$_foot"
    check_toplevel_capture
    check_xdpw
    check_ipc_identifier
    check_activation
  else
    check pixdecor 1 "foot never mapped"; check filters 1 "foot never mapped"
    check enclave-mark 1 "foot never mapped"
    check ipc-context 1 "foot never mapped"
    check toplevel-capture 1 "foot never mapped"
    check ipc-identifier 1 "foot never mapped"
    check activation 1 "foot never mapped"
  fi
  client dialog python3 -I "$HERE/test/live-dialog"; DLG_PID=$!
  check_ipc_guard
  _live_stop
}

# Session 4: pixdecor with shadows and rounded corners, and an enclave window
# it decorates. pixdecor counts its shadow in the view geometry, which once
# made the frame float a shadow-width off the window.
session_hug() {
  { printf '%s\n' '[core]' "plugins = $PLUGINS" \
      'preferred_decoration_mode = server' 'xwayland = false' '' \
      '[pixdecor]' 'border_size = 4' 'overlay_engine = rounded_corners' \
      'shadow_radius = 8' 'rounded_corner_radius = 12' '' \
      '[security-context-v1]' \
      "allowed_protocols = $(printf '%s' "$ALLOW zxdg_decoration_manager_v1" \
        | sed 's/ /, /g')" '' \
      '[enclave-mark]' 'thickness = 6' 'colors = titan=1.0 0.0 1.0 1.0' '' \
      '[output:HEADLESS-1]' 'mode = 1280x720@60000'
  } > "$T/hug.ini"
  start_wayfire "$T/s4" "$T/hug.ini" 1
  client secctx "$T/secctx" "$S/ctx.sock" titan; SC_PID=$!
  until_true "grep -q ready '$S/secctx.log'"
  WAYLAND_DISPLAY=$S/ctx.sock XDG_RUNTIME_DIR=$RUN foot -c "$T/foot.ini" \
    --app-id wfm-hug sleep 600 > "$S/hug.log" 2>&1 &
  ENC_PID=$!
  if ! _h=$(wait_view "v['app-id'] == 'wfm-hug'"); then
    check mark-hugs 1 "the enclave foot never mapped"; _live_stop; return
  fi
  place "$(printf '%s\n' "$_h" | jpy "d['id']")" 100 80 800 500
  sleep 1
  check_mark_hugs "$(printf '%s\n' "$_h" | jpy "d['id']")"
  _live_stop
}

# check_mark_hugs <view id>: probe the frame against the VISIBLE window,
# which is the client plus pixdecor's border (4), without its shadow
check_mark_hugs() {
  _g=$(ipc window-rules/view-info "{\"id\": $1}" | jpy "'%d %d %d %d %d %d' % (
    d['info']['base-geometry']['x'], d['info']['base-geometry']['y'],
    d['info']['base-geometry']['width'], d['info']['base-geometry']['height'],
    d['info']['base-geometry']['x'] - d['info']['geometry']['x'] - 4,
    0)")
  # shellcheck disable=SC2086  # six numbers
  set -- $_g
  _vl=$(($1 - 4)); _vr=$(($1 + $3 + 4)); _vb=$(($2 + $4 + 4)); _sh=$5
  _mid=$(($2 + $4 / 2))
  _in=$(at $((_vl - 3)) "$_mid")            # in the frame, on the edge
  _old=$(at $((_vl - _sh - 3)) "$_mid")     # where a shadow-width-out frame was
  _edge=$(at $((_vr + 5)) $((_vb - 30)))    # right edge, clear of the corner
  _corner=$(at $((_vr + 5)) $((_vb + 5)))   # the square corner's own pixel
  _d="shadow $_sh; edge [$_in] shadow-out [$_old] right [$_edge]"
  _d="$_d corner [$_corner]"
  if [ "$_in" = "$MARK" ] && [ "$_old" != "$MARK" ] && \
     [ "$_edge" = "$MARK" ] && [ "$_corner" != "$MARK" ]; then
    check mark-hugs 0 "$_d"
  else
    check mark-hugs 1 "$_d (mark is [$MARK])"
  fi
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

# Session 5: edge-warp. HEADLESS-2 at scale 2 is 640x360 logical beside
# HEADLESS-1's 1280x720, so HEADLESS-1's right edge has nothing beyond it
# below y=360. stipc feeds RELATIVE motion, which goes through wlroots'
# clamping exactly as a mouse does. edge-flipping is loaded too, with pointer
# flips ON (its default is off) and no approach detection, so any sample at
# an outer edge flips: the independence check needs a flipper that WOULD.
# A 9x9 wrapping grid, because on a 3-wide one three flips land back where
# they started and read as none (which is how the first draft passed a
# planted regression).
session_warp() {
  printf '%s\n' '[core]' \
    'plugins = ipc ipc-rules stipc edge-warp edge-flipping' \
    'xwayland = false' 'vwidth = 9' 'vheight = 9' '' \
    '[edge-flipping]' 'approach_zone_px = 0' 'max_approach_ms = 1000' \
    'enable_pointer_flip = true' 'wrap = true' '' \
    '[output:HEADLESS-1]' 'mode = 1280x720@60000' 'position = 0,0' '' \
    '[output:HEADLESS-2]' 'mode = 1280x720@60000' 'position = 1280,0' \
    'scale = 2' > "$T/warp.ini"
  start_wayfire "$T/s5" "$T/warp.ini" 2
  # Spaced past edge-flipping's 100ms tick: it samples the pointer on that
  # tick, so moves arriving faster are never judged at the edge at all.
  _wm() { ipc stipc/move_cursor "{\"x\": $1, \"y\": $2}" > /dev/null
    sleep 0.15; }
  _wc() { ipc window-rules/get_cursor_position \
    | jpy "'%d %d' % (int(d.get('pos', d)['x']), int(d.get('pos', d)['y']))"; }
  _ws() { ipc window-rules/list-outputs \
    | jpy "' '.join('%d,%d' % (o['workspace']['x'], o['workspace']['y'])
           for o in d)"; }
  _set() { ipc wayfire/set-config-options "{\"edge-warp/$1\": \"$2\"}" \
    > /dev/null; }
  _wa=$(_ws); _wm 640 400; _wm 0 400; _wm 640 400; _wb=$(_ws)
  [ "$_wa" != "$_wb" ]
  check edge-flip-armed $? "one sample at HEADLESS-1's outer left edge flips:
    workspaces $_wa -> $_wb"
  # The landing is INSET 16 from HEADLESS-2's last row, which is its outer
  # edge: a further sample there (the nudge) must flip nothing. Landing ON
  # that row did, in the first version of the plugin.
  _w0=$(_ws)
  _wm 640 400; _wm 1279 600; _wm 1300 600; _at=$(_wc); _wm 1301 343
  _w1=$(_ws)
  [ "$_at" = "1300 343" ] && [ "$_w1" = "$_w0" ]
  check edge-warp $? "pushed 21px right at y=600 (nothing beyond): at $_at,
    want 1300 343 on HEADLESS-2, 16 in from its edge; workspaces $_w1 (want
    $_w0, the landing flips nothing)"
  _set enabled false
  _wm 640 400; _wm 1279 600; _wm 1300 600; _wm 1300 600; _at=$(_wc)
  _w2=$(_ws)
  [ "$_at" = "1279 600" ] && [ "$_w2" = "$_w0" ]
  check edge-warp-off $? "switched off: at $_at (want 1279 600, a wall);
    workspaces $_w2 (want $_w0, no flip)"
  _set enabled true; _set proportional true
  _wm 640 400; _wm 1279 600; _wm 1300 600; _at=$(_wc)
  [ "$_at" = "1300 300" ]
  check edge-warp-prop $? "proportional: at $_at, want 1300 300 (600/720)"
  alive || died edge-warp
  _live_stop
}

# Session 6: output-commit-backoff. HEADLESS-1's every frame fails its test
# while $T/fail exists, as an undocked monitor's does before its hotplug is
# read, and a foot printing every 50ms keeps damaging it. The shim counts
# each failure into $T/fail.count. animate's startup fade runs throughout:
# its pre-render hook asks for a repaint INSIDE every paint, the shape any
# running animation has, so the retry timer alone would not break the loop
# (a build without paint()'s early return livelocks here; proven). The
# trigger is removed BEFORE teardown: a
# livelocked compositor cannot even handle SIGTERM (its signals come through
# the same starved event loop), and with the cause gone stock recovers too.
session_backoff() {
  printf '%s\n' '[core]' 'plugins = ipc ipc-rules animate' \
    'xwayland = false' '' '[animate]' 'startup_duration = 120000ms linear' \
    '' '[output:HEADLESS-1]' 'mode = 1280x720@60000' > "$T/backoff.ini"
  WF_ENV="LD_PRELOAD=$T/failtest.so WFM_FAILTEST=$T/fail"
  WF_ENV="$WF_ENV WFM_FAILTEST_OUTPUT=HEADLESS-1"
  start_wayfire "$T/s6" "$T/backoff.ini" 1
  WF_ENV=
  client tick foot -c "$T/foot.ini" --app-id wfm-tick \
    sh -c 'while :; do date +%s.%N; sleep 0.05; done'
  FOOT_PID=$!
  wait_view "v['app-id'] == 'wfm-tick'" > /dev/null \
    || { check backoff-alive 1 "the redrawing window never mapped"
         _live_stop; return; }
  _n() { wc -c < "$T/fail.count" 2>/dev/null || echo 0; }
  _q() { timeout 3 python3 -I "$HERE/test/live-ipc" "$SOCK" "$1" \
    > /dev/null 2>&1; }
  : > "$T/fail"; sleep 0.5
  _c0=$(_n); _t0=$(date +%s%N)
  _ok=0
  for _m in window-rules/list-outputs window-rules/list-views \
            window-rules/list-outputs; do
    _q "$_m" || _ok=1
  done
  sleep 1
  _c1=$(_n); _t1=$(date +%s%N)
  check backoff-alive "$_ok" "IPC answered 3 calls while HEADLESS-1 failed
    every frame (stock: timeouts, it never polls again)"
  _ms=$(( (_t1 - _t0) / 1000000 )); _f=$((_c1 - _c0))
  _ok=1
  [ "$_f" -gt 0 ] && [ "$_f" -le $(( _ms * 3 / 32 + 5 )) ] && _ok=0
  check backoff-rate "$_ok" "$_f failures in ${_ms}ms (want 1 to ~1 per 16ms;
    stock: thousands)"
  rm -f "$T/fail"; sleep 1
  _c2=$(_n); sleep 1; _c3=$(_n)
  WAYLAND_DISPLAY=$WLD XDG_RUNTIME_DIR=$RUN timeout 5 \
    grim -o HEADLESS-1 "$S/shot.png" > "$S/grim.log" 2>&1
  _g=$?
  _ok=1
  [ "$_c3" = "$_c2" ] && [ "$_g" = 0 ] && _ok=0
  check backoff-recover "$_ok" "trigger removed: failures $_c2 -> $_c3 a second
    later (want no change), grim rc $_g (want 0)"
  alive || died backoff-alive
  _bp=$WF_PID
  _live_stop
  sleep 0.5; kill -KILL "$_bp" 2>/dev/null || :
}

# Session 7: the unplug, nested. A wayfire on the wayland backend inside a
# headless one: its outputs WL-1 and WL-2 are windows of the parent, and
# closing one makes wlroots destroy that output, the backend-side removal a
# hotplug is (so this checks the event that ENDS the real undock gets read,
# where backoff only took the fault away). The shim fails WL-2's every frame
# and stays on through the close; animate's fade damages every output
# without a client. Pixman, so the nested renderer needs nothing of the
# parent but shm.
session_unplug() {
  printf '%s\n' '[core]' 'plugins = ipc ipc-rules' 'xwayland = false' '' \
    '[output:HEADLESS-1]' 'mode = 2560x1440@60000' > "$T/parent.ini"
  start_wayfire "$T/s7" "$T/parent.ini" 1
  _psock=$SOCK
  printf '%s\n' '[core]' 'plugins = ipc ipc-rules animate' \
    'xwayland = false' '' '[animate]' 'startup_duration = 120000ms linear' \
    > "$T/nested.ini"
  _nsock=$S/nested.sock
  env -u DISPLAY -u WAYFIRE_SOCKET XDG_RUNTIME_DIR="$RUN" \
    WAYLAND_DISPLAY="$WLD" WLR_BACKENDS=wayland WLR_WL_OUTPUTS=2 \
    WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 _WAYFIRE_SOCKET="$_nsock" \
    LD_PRELOAD="$T/failtest.so" WFM_FAILTEST="$T/unplug" \
    WFM_FAILTEST_OUTPUT=WL-2 XDG_DATA_HOME="$T/data" \
    "$P/bin/wayfire" -c "$T/nested.ini" > "$S/nested.log" 2>&1 &
  _np=$!
  _q() { timeout 3 python3 -I "$HERE/test/live-ipc" "$1" "$2" ${3:+"$3"} \
    2>/dev/null; }
  _outs() { _q "$_nsock" window-rules/list-outputs \
    | jpy "' '.join(o['name'] for o in d)"; }
  _n() { wc -c < "$T/unplug.count" 2>/dev/null || echo 0; }
  _i=0; _o=
  until [ "$_o" = "WL-1 WL-2" ]; do
    _i=$((_i + 1)); _o=$(_outs 2>/dev/null)
    [ "$_i" -lt 50 ] || break
    sleep 0.2
  done
  [ "$_o" = "WL-1 WL-2" ] || { check unplug-removed 1 "the nested wayfire
    never came up with WL-1 and WL-2 (got '$_o')"
    kill -KILL "$_np" 2>/dev/null; _live_stop; return; }
  : > "$T/unplug"; sleep 1
  _c0=$(_n)
  _vid=$(_q "$_psock" window-rules/list-views | jpy "next(v['id'] for v in d
    if (v.get('title') or '').endswith('WL-2'))")
  _q "$_psock" window-rules/close-view "{\"id\": $_vid}" > /dev/null
  _i=0; _o=
  until [ "$_o" = WL-1 ] || [ "$_i" -ge 15 ]; do
    _i=$((_i + 1)); sleep 0.2; _o=$(_outs 2>/dev/null)
  done
  _ok=1
  [ "$_c0" -gt 0 ] && [ "$_o" = WL-1 ] && _ok=0
  check unplug-removed "$_ok" "WL-2 failing ($_c0 failures before the
    close), its window closed with the fault still on: nested outputs now
    '$_o' (want WL-1; stock never reads the removal)"
  _c1=$(_n); sleep 1; _c2=$(_n)
  [ "$_c1" = "$_c2" ]
  check unplug-quiet $? "failures $_c1 -> $_c2 a second later (want none:
    the failing output is gone)"
  rm -f "$T/unplug"; sleep 0.5
  kill "$_np" 2>/dev/null; _bp=$WF_PID
  _live_stop
  sleep 0.5; kill -KILL "$_np" "$_bp" 2>/dev/null || :
}

# Session 8: ipc-name-dropped-client. A client renames itself (prctl) so its
# name is unmistakable, sends requests and never reads a reply, until its
# socket buffer is full and wayfire drops it. The log line must carry ITS
# pid and name, and the reason. It stays alive (sleeping) while that
# happens, so the name is still there to be read.
session_ipcname() {
  printf '%s\n' '[core]' 'plugins = ipc ipc-rules' 'xwayland = false' '' \
    '[output:HEADLESS-1]' 'mode = 1280x720@60000' > "$T/ipcname.ini"
  start_wayfire "$T/s8" "$T/ipcname.ini" 1
  python3 -I - "$SOCK" > "$S/slow.out" 2>&1 <<'SLOWPY' &
import ctypes, json, os, socket, struct, sys, time
ctypes.CDLL(None).prctl(15, b"wfm-slowreader", 0, 0, 0)   # PR_SET_NAME
print(os.getpid(), flush=True)
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
body = json.dumps({"method": "window-rules/list-outputs", "data": {}})
msg = struct.pack("=I", len(body)) + body.encode()
s.setblocking(False)
for _ in range(20000):           # never read: the replies pile up
    try:
        s.send(msg)
    except BlockingIOError:
        time.sleep(0.01)
    except OSError:
        break                    # dropped: the point
time.sleep(5)
SLOWPY
  _sp=$!
  _i=0
  until grep -q 'Error sending json to client' "$S/wf.log" 2>/dev/null \
      || [ "$_i" -ge 100 ]; do
    _i=$((_i + 1)); sleep 0.1
  done
  _pid=$(head -1 "$S/slow.out" 2>/dev/null)
  _line=$(grep -m1 'Error sending json to client' "$S/wf.log" 2>/dev/null)
  case $_line in
    *"client pid $_pid (wfm-slowreader): Resource temporarily unavailable;"*)
      check ipc-name 0 "named: ${_line#*client }" ;;
    *) check ipc-name 1 "want 'pid $_pid (wfm-slowreader): Resource
    temporarily unavailable', got: '${_line:-no drop logged}'" ;;
  esac
  kill "$_sp" 2>/dev/null; alive || died ipc-name
  _live_stop
}

write_fixtures
session_scale
session_main
session_hug
session_gpu
session_warp
session_backoff
session_unplug
session_ipcname
[ "$FAILS" = 0 ] || fail "$FAILS live check(s) failed against $P"
pass "$CHECKS checks against $P"
