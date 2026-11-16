# xdpw-window-restore.patch

Let a screencast session's restore data name a WINDOW, not only an output,
and honour an app's request for a TRANSIENT restore by default. Three files
(`include/screencast_common.h`, `src/screencast/screencast.c`,
`src/screencast/wlr_screencast.c`). Applies after `xdpw-window-only`.

## Why

Chrome shares through TWO portal sessions: one for the preview in its own
"Choose what to share" dialog, then a second for the real capture. It asks
the first to persist (`persist_mode` 1, transient) and hands the returned
restore data to the second, so the second needs no chooser.

0.8.1 (and upstream master as of 2026-11-15) breaks that twice over:

- after a chooser runs, it forces `persist_mode` to NONE unless
  `XDPW_PERSIST_MODE` is set in its environment, so no restore data is
  returned at all;
- even when it is, restore data is written and read for an OUTPUT only
  (`output_name`), so a window can never be restored.

So every window share asked the person twice: once for Chrome's preview, and
again when they pressed Share. Seen live in an enclave's Meet on 2026-11-15,
where the second picker, closed as a glitch, logged `wlroots: no output
found` and the share did not start.

## What it changes

- Restore data may carry `toplevel_identifier` (the ext-foreign-toplevel
  identifier, which the compositor keeps stable for that window's lifetime)
  in place of `output_name`, in the same version-1 dictionary. An older
  backend skips the unknown key with a warning, so nothing is broken by it.
- `xdpw_wlr_target_from_data` restores a window by that identifier, and
  declines (falling back to the chooser) when the window has gone.
- After a chooser, an app's own `persist_mode` is honoured up to TRANSIENT
  when `XDPW_PERSIST_MODE` is unset. A transient token lives only as long as
  the app does and restores only the window the person already chose, which
  is exactly Chrome's preview-then-share. PERMANENT still needs
  `XDPW_PERSIST_MODE=permanent`, as before: nothing here lets an app keep a
  choice past its own lifetime.

## Measured (dragonfly, 2026-11-15)

Nested headless wayfire, a foot window, a private pipewire, the backend on a
private bus: session 1 selects the window with `persist_mode` 1, session 2 is
handed session 1's restore data.

    without this patch   restore no  (no restore data returned)
    with it              restore yes, reprompt 0 (the chooser did not run),
                         restored consumed 51 696x494 (the window's own size)

`test/live.t` xdpw-window-only asserts the second, through the sandbox
socket an enclave uses.
