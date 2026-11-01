# wayfire-xdg-activation-sandbox.patch

Makes Wayfire's `xdg-activation` plugin respect security-context-v1
sandboxes (needs `wayfire-security-context-allowlist.patch` for the
context lookup). Without any security contexts, behaviour is unchanged,
which is what makes it upstreamable.

## The rule

Each token is attributed, when it is created, to the client of the surface
it was created from (wlroots keeps the creating client private, and clears
it before announcing the token; the surface is public, and wlroots has
already checked the input serial against that same client). Then, when the
token is used to activate a window:

| token created by       | target in sandbox X | target unsandboxed |
|------------------------|---------------------|--------------------|
| sandbox X              | honoured            | denied             |
| another sandbox        | denied              | denied             |
| unsandboxed client     | honoured            | honoured           |
| unknown (no surface)   | denied              | honoured           |

Focus carries the user's keystrokes, so the direction that matters most is
focus moving INTO a sandbox: an unattributed token is refused there and
keeps upstream behaviour elsewhere.

## Verified

`test/live.t` (`activation`) runs all seven cases with real tokens (a
virtual keyboard supplies input serials) and gets `accaaca`; a stock build,
and ours with only the stock security-context plugin, honour all seven.
