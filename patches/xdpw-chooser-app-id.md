# xdpw-chooser-app-id.patch

Add each window's app id to the line a `dmenu` chooser is handed. One file
(`src/screencast/chooser.c`). Applies after `xdpw-window-restore`.

    before   Window: <title> (<identifier>)
    after    Window: <title> (<identifier>) [<app id>]

## Why

A chooser can only show what it is told, and the title alone makes a poor
picker: severance's enclave chooser wants each window's app id to look up
the app's NAME and ICON in the enclave's own desktop entries. The backend
already has it (`toplevel->app_id`, from ext-foreign-toplevel-list), so
passing it on is one format string.

## Why it is safe

The chooser prints back the line it chose and the backend matches it byte
for byte against a label it regenerates with the SAME function, so both
ends change together. A chooser that parses the old form must accept the
bracketed suffix; severance's does, and still accepts the old form. A plain
dmenu-style chooser (wmenu, fuzzel) simply shows the app id as text. A
window with no app id gets `[]`.

## Measured (dragonfly, 2026-11-15)

`test/live.t` xdpw-window-only compares the offered lines with what a client
in the same security context lists for itself, as (identifier, app id)
PAIRS, so a label that loses or mangles its app id fails there.
