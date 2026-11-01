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
sees exactly the configured list, and a plain client still sees screencopy.
The same check fails on a stock build.
