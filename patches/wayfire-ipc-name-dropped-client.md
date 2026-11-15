# wayfire-ipc-name-dropped-client.patch

When wayfire drops an IPC client because a send to it failed, the log line
names the client and the reason:

    stock    Error sending json to client!
    patched  Error sending json to client pid 4242 (usher-watch): Resource
             temporarily unavailable; disconnecting it

One file (`plugins/ipc/ipc.cpp`), every hunk tagged
`wayfire-mods (ipc-name-dropped-client)`. Behaviour is unchanged: the client
is still dropped exactly as before. Only the message differs, and its
prefix is kept so a grep for `Error sending json to client` still matches.

## Why

wayfire makes every IPC client socket non-blocking and, when a write to one
fails, shuts the connection down. A client that stops reading for a moment
during a burst of events (a login mapping two accounts' windows) loses its
connection. usher met this for weeks as `watch loop exit: Failed to read
anything from the socket!` with nothing on wayfire's side to say which client
had been dropped or why (usher.md; the cause was found 2026-11-14, see
`_usher-requests-wayfire-mods.md` W1). The same line appears in sessions
where usher was fine, so other clients are being dropped too, and this is
what will say which.

WHO comes from `SO_PEERCRED` on the socket (the peer's pid as it connected)
and `/proc/<pid>/comm` (its process name, `?` if it has gone). It is asked
only after a send has failed, so it costs nothing on the normal path. WHY is
`errno` from the failed write: `Resource temporarily unavailable` (EAGAIN)
is a client that stopped reading, `Broken pipe` one that has gone away.

## Proven

`test/live.t` session `ipcname`: a client renames itself `wfm-slowreader`,
sends requests and never reads a reply, so its buffer fills and wayfire drops
it. The check requires the line to name that pid, that name and EAGAIN.
Against the installed build without this patch it FAILS (the bare stock
line), and every other check passes.

## Remove when

Upstream wayfire names the client in this message, or stops dropping slow
clients (buffering instead), at which point the line no longer happens. Drop
it from `WAYFIRE_PATCHES` in `libexec/build-wayfire`, delete this pair,
remove `session_ipcname` (its call and the `ipc-name` header row) from
`test/live.t`, and its name from README.md's patch list.
`git grep ipc-name-dropped-client` then finds nothing.
