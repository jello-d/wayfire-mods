# wayfire-output-commit-backoff.patch

When an output's frame fails its test or commit, retry it from a 16ms timer
instead of at once, and draw nothing on that output until the timer fires.
One file (`src/output/render-manager.cpp`), every hunk tagged
`wayfire-mods (output-commit-backoff)`.

## Why

Undocking dragonfly (a Thunderbolt dock driving two monitors over DP MST)
froze the desktop on 2026-11-14 until the power was cut. The kernel was
fine (the journal ran 40s past the unplug); the compositor was not. Its log
held 16473 `Output test failed!` lines, one every ~1.4ms from the unplug's
own millisecond to the end, no output-removed line, and every IPC client
timed out.

Stock `paint()` answers a failed commit with `damage_whole()`, which asks
for a repaint at once: `wlr_output_schedule_frame()`, an IDLE callback. And
libwayland's `wl_event_loop_dispatch_idle()` runs idle callbacks WHILE the
list is non-empty, so a frame that fails and schedules the next never lets
the loop poll its file descriptors again. The output fails because its
connector is already gone; the udev hotplug that would remove it is one of
those unread descriptors. So the loop starved the one event that could have
ended it, along with IPC and input (no VT switch either).

A timer is a file descriptor, run only after the loop has polled, so the
hotplug is read and the dead output removed. The early return while the
timer is armed keeps any other repaint request (a client, a plugin's
animation) from re-entering the immediate path during the wait. The damage
is kept, so the retry draws the whole output.

Racy in the wild: only a frame landing on the output between the kernel
removing its connector and wlroots hearing of it. A transient failure (the
`Atomic commit failed: Device or resource busy` that shows up now and then)
costs one frame instead of an immediate retry.

## Proven

`test/live.t` session `backoff`: a preloaded shim (`test/live-failtest.c`)
makes one headless output's test fail while a trigger file exists, under a
window that redraws continuously and animate's startup fade (a hook that
asks for a repaint inside every paint, as any running animation does).
Three builds of the same pins, measured 2026-11-14:

    patched           20/20 live checks; 64-71 failures a second, IPC fine
    stock             backoff-alive and -rate FAIL: IPC timed out, ~10500
                      failures a second (16473 in 33s on the real undock)
    timer, no gate    the same two FAIL (~9800 a second): without paint()'s
                      early return the fade's in-paint repaint request
                      rebuilds the idle loop, so both halves are needed

All three recover once the trigger is removed (backoff-recover), which is
what shows the shim, and not something else, causes the hang. The other
17 checks pass on the patched build, the real-GPU capture included.

Session `unplug` is the undock itself, nested: a wayfire on the wayland
backend inside a headless one, its WL-2 failing every frame, then WL-2's
window closed from the parent, which is wlroots' backend destroying the
output the way a hotplug does. With the fault STILL on, patched drops WL-2
and its failures stop (22/22, three runs in a row); stock never reads the
removal, IPC dead, ~3000 failures a second. That is the half backoff could
not show: the event that ENDS the real undock gets read.

## Remove when

Upstream wayfire stops repainting from an idle callback after a failed
commit (`paint()`'s `if (!committed)` branch in
`src/output/render-manager.cpp`). Drop it from `WAYFIRE_PATCHES` in
`libexec/build-wayfire`, delete this pair and `test/live-failtest.c`,
remove `session_backoff` and `session_unplug` (their calls, the
`backoff-*` and `unplug-*` header rows, and `build_failtest`) from
`test/live.t`, and its name from README.md's patch
list. `git grep output-commit-backoff` then finds nothing.
