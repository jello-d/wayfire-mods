# wayfire-diag-focus-surface.patch (TEMPORARY DIAGNOSTIC)

Makes wayfire's `Attempting to give focus to a view without focus surface!`
warning name the view (id, app-id, mapped) and print a short backtrace, so
the caller of that path identifies itself.

## Why

Measured on manifold at the 2026-11-01 login, the first session on
wayfire e268d4e5: four such warnings in 5ms, 15ms after usher's first
restored kitty window (view 26) was created and before it mapped. The old
session never logged it. Not reproduced headless with kitty alone, Chrome,
a virtual keyboard, Xwayland or usher-style IPC placement; ruled out by
reading: window rules (none), follow-focus, focus-steal-prevent,
workspace-switch refocus, xdg-activation (it logs its own refusals),
waybar's taskbar and mux's focus hook (both act only on mapped views or
clicks). Proven to fire: an IPC `focus-view` on a never-mapped window logs
the view and the chain `ipc_rules_t::focus_view -> focus_request ->
focus_raise_view` (that call also answers `{"result": "ok"}` having done
nothing).

## Remove when

The next occurrence names its caller. Drop this patch from
`WAYFIRE_PATCHES` and delete it; it changes no behaviour.
