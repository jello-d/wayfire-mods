# wayfire-ipc-security-context.patch

Adds a view's security context to wayfire's IPC view description, so an IPC
client can tell which sandbox (if any) a window belongs to. Requested by
usher (U-W1 in `_wayfire-mods-requests-usher.md`): with two accounts' Chrome
windows on one display, nothing else the IPC reports tells them apart.

## The contract

Every view description (`window-rules/view-info`, `list-views`, and the
view events, which all go through `view_to_json`) gains:

    "security-context": {"sandbox-engine": ..., "app-id": ...,
                         "instance-id": ...}

- ABSENT when the view's client did not connect through a security context.
- PRESENT, with the values the sandbox engine committed, when it did.
- PRESENT WITH EMPTY STRINGS when the engine committed a context and named
  nothing. So "unsandboxed" is never confused with "sandboxed, unnamed".

It is a read: no capability changes for anyone. The lookup is the one SV8
added (`wayfire/plugins/common/security-context.hpp`), so this needs
`wayfire-security-context-allowlist.patch` first, and the
`security-context-v1` plugin loaded; without that plugin every view reads as
absent. It matches what `enclave-mark` colours by, since both read the same
context, which a uid cannot (an owner-uid client can connect through an
enclave socket).

## Verified

`test/live.t` (`ipc-context`): a personal window reads absent, a window
through a named context reads `engine|app-id|instance`, and one through a
context that set nothing reads three empty fields. The deployed build
without this patch fails it (absent for all three).
