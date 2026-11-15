# xdpw-upstream-896cee8.patch

Upstream xdg-desktop-portal-wlr commit 896cee8, "Fix screensharing on
pipewire 1.6.x", taken verbatim (`git show`), made after the 0.8.1 we pin.
Drop it when the pin moves past it.

## Why

0.8.1 offers a WINDOW cast at framerate 0/1, with a maxFramerate range
whose minimum (1) is above its maximum (0). pipewire 1.6 cannot intersect
that with any consumer's format, so every link is refused:

    pw.link: ... no more output formats

and WirePlumber logs `link failed`. Start still succeeds and returns a
stream, so the portal reports a share and the browser shows nothing. That
is the "flaky, broken" enclave window share of 2026-11-15 (titan's Meet on
dragonfly, two attempts in the journal at 10:35, both refused this way).

The commit skips the framerate when it is 0 in `fixate_format` and
`build_format`, and guards the division in the param handler.

## Measured (dragonfly, 2026-11-15)

Nested headless wayfire, a foot ticker window, a private pipewire, the
backend on a private bus, and `test/live-pwconsume` linking itself to the
node Start returned:

    window-only patch alone   consume-error no more output formats
    + this commit             consumed 51 696x494 (the window's own size)

`test/live.t` xdpw-window-only asserts the second.
