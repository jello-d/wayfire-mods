# wayfire-ipc-toplevel-identifier.patch

Adds a window's ext-foreign-toplevel-list-v1 identifiers to wayfire's IPC
view description, so an IPC client and a Wayland client of that list can
name the SAME window without its title. Requested by usher (U-W2 R2 in
`_wayfire-mods-requests-usher.md`): an account reports `identifier ->
identity` for its own windows, and the owner joins on the identifier it
reads over IPC. Until now that join was a digest of the live title, which
an app rewrites at will and two windows can share.

## The contract

Every view description (`window-rules/view-info`, `list-views`, and the
view events, which all go through `view_to_json`) gains:

    "toplevel-identifier": "<32 hex>"        the PERSONAL list's handle
    "security-context": {..., "toplevel-identifier": "<32 hex>"}
                                             the window's INSTANCE list's

- Each list handle carries its OWN identifier (wlroots mints a random token
  per handle), and our `toplevel-capture` puts a sandboxed window on two
  lists: the personal one (clients outside any context) and its instance's.
  So a sandboxed client sees the INSTANCE identifier, and that is the one
  to join on. The two are always different.
- The instance key is absent for a window outside any security context.
- Both are absent while no list holds the window (unmapped, or
  `toplevel-capture` not loaded).
- Stable for the life of the handle, which is map to unmap, as the
  protocol documents. A window unmapped and mapped again is a new handle.

The patch adds only the header
(`wayfire/plugins/common/toplevel-identifier.hpp`, a custom_data_t stored
on the view under `toplevel-identifier`) and the reader in `view_to_json`;
our `toplevel-capture` plugin is the writer. It needs
`wayfire-ipc-security-context.patch` first (it extends that block). It is a
read: no capability changes for anyone, and a client learns nothing it
was not already sent by its own list.

## Verified

`test/live.t` (`ipc-identifier`): for an enclave window, the top-level key
equals what a plain client's list was sent and the security-context key
equals what the enclave client's list was sent, and they differ; a personal
window has the first and not the second; and both are already present in
the `view-mapped` event. FAILS against the deployed build without the patch
(both keys empty), and against a mutant that swaps the two identifiers.
