# wayfire-security-context-allowlist.patch

Extends Wayfire's `security-context-v1` plugin, which already implements the
protocol but filters sandboxed clients by a DENYLIST
(`privileged_protocols`, four names by default).

## What it changes

- New option `security-context-v1/allowed_protocols`. When non-empty, a
  client that connected through a security context sees ONLY the listed
  globals, whatever its sandbox engine. Empty (the default) keeps upstream's
  behaviour exactly, which is what makes this upstreamable.
- Both lists are trimmed of whitespace around each comma-separated name.
- A new installed header, `wayfire/plugins/common/security-context.hpp`,
  exposes a client's context (sandbox engine, app_id, instance_id) to other
  plugins through `wf::shared_data`, so a decorator or a capture source can
  key on which sandbox a window belongs to.
- In the same header, `instance_scope_t` (its own shared type, so a plugin
  built against the older header cannot allocate the registry short): a
  plugin registers resolvers that name the sandbox INSTANCE a global was made
  for. The filter shows such a global to that instance's clients only,
  whatever the allowlist says, and to nobody else, sandboxed or not.
  `toplevel-capture` uses it for each enclave's own screen capture, so the
  allowlist never names `ext_output_image_capture_source_manager_v1` and a
  compositor without that plugin offers an enclave no screen at all.

## Why an allowlist

A denylist fails open on every upgrade. Measured on the stock plugin at
wayfire e268d4e5: a sandboxed client still sees layer-shell, virtual keyboard
and pointer, `ext_image_copy_capture` with output capture, export-dmabuf,
`ext_data_control_manager_v1` (the default list names only the older `zwlr_`
data-control), gamma, output management and power, and the security-context
manager itself. Each protocol added by a later Wayfire or wlroots joins that
list silently. With an allowlist, a new protocol stays hidden until listed.

## Verified

`test/live.t` (`security-ctx`): a client connecting through a context socket
sees exactly the configured list plus its own scoped screen global, and a
plain client still sees screencopy. The same check fails on a stock build.

Measured 2026-11-15, by plugin combination (screen-capture globals an
enclave socket / a personal socket sees): both new 1/1 (the enclave's own);
no `toplevel-capture` 0/1; this patch absent (old filter) 0/1; the name
wrongly allowlisted with both new 1/1 (still its own). Only the name
allowlisted AND `toplevel-capture` absent shows an enclave the real one,
which tackup's tests refuse (WM4).
