# xdpw-window-only.patch

Let xdg-desktop-portal-wlr capture WINDOWS when the compositor offers the
per-window ext capture source but not the whole-output one. Two files
(`src/screencast/wlr_screencast.c`, `src/screencast/ext_image_copy.c`).

## Why

Upstream (0.8.1, and master as of 2026-11-14) takes the
ext-image-copy-capture path only when BOTH
`ext_image_copy_capture_manager_v1` AND
`ext_output_image_capture_source_manager_v1` are bound, and otherwise falls
back to wlr-screencopy or exits:

    Compositor supports neither ext_image_copy_capture or wlr_screencopy!

The protocol does not need the output source to capture a window; that is
`ext_foreign_toplevel_image_capture_source_manager_v1`'s job. A sandboxed
client is exactly the case where the difference matters: a severance
enclave's security-context socket offers the per-window set (an enclave may
share its OWN windows) and withholds the output source (it must never
capture the whole screen, which shows the owner's windows). Stock exits
there, systemd restarts it until the frontend's 25s activation timeout, and
every portal client in the enclave (Chrome at login) waits those 25s.

## What it changes

- `xdpw_ext_ic_usable()` is the one predicate for "capture through ext",
  used by the init gate and all three per-cast dispatches, so they cannot
  disagree. It is upstream's predicate PLUS window-only, and window-only is
  taken only when wlr-screencopy is absent: a compositor upstream could
  already serve keeps upstream's path exactly.
- With no output source, MONITOR is cleared from `AvailableSourceTypes`, so
  a client is never offered a source nobody can capture.
- `xdpw_ext_ic_session_init` refuses a target whose source manager is
  missing (a restored session, or the `none` chooser, which always picks an
  output), rather than reaching the `assert(source != NULL)`.

## Measured (dragonfly, 2026-11-14)

On a private bus, against the live enclave socket and the owner's:

    stock,   enclave socket   exits 1, "supports neither"
    patched, enclave socket   AvailableSourceTypes=2 (window)
    patched, owner socket     AvailableSourceTypes=3 (monitor + window)
    stock,   owner socket     AvailableSourceTypes=3

`test/live.t` (xdpw-window-only) repeats the first three rows against the
nested compositor on every `setup.sh verify`.

Worth offering upstream: nothing here is specific to wayfire.
