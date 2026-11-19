# filters-view-output.patch

A per-view shader's node keeps its hook on the output its window is on NOW,
not the one it was on when the shader was set, and lets go of an output
before that output is freed. One file (`src/filters.cpp`), applied after
`filters-frame-margins.patch`, every change tagged
`wayfire-mods (filters-view-output)`.

## Why

dragonfly, 2026-11-19 16:01:52, on an undock (hwdp W4,
`_wayfire-mods-requests-hwdp.md`): a fatal SIGSEGV in

    wf::render_manager::rem_effect  <- ~wf_filters  <- ipc_set_view_shader

usher re-placed the windows of the removed panel onto eDP-1 and re-applied
the invert shader to one Chrome window with `set_view_shader`. Replacing the
shader destroys the old `wf_filters` node, and that destructor handed its
fade hook back to the output it had cached at construction, which the undock
had already freed.

`wf_filters` took `view->get_output()` once, added its `pre_hook` to that
output's render manager, and never looked again: not when the window moved
to another output, not when the output went away. While the window lived on,
the hook also kept running on (and damaging through) the wrong output. And
`output` had no initialiser, so a view with no output at that moment left a
garbage pointer for the destructor.

## What it does

- `output` starts as nullptr; `attach(to)` moves `pre_hook` from the old
  output to the new one, and is the only thing that touches either.
- The node follows its view: every output change goes through
  `view_interface_t::set_output`, which emits `view_set_output_signal` on
  the view, so one connection covers a move, the transfer on an output's
  removal, and a workspace set reattached later.
- On `output_pre_remove_signal` for its output it lets go first. In the
  wayfire this is pinned to, that is a BACKSTOP: removal re-homes every view
  with `set_output` before the output is destroyed, so following the view
  already lets go in time (measured: a mutant without this listener passes).
  It makes the guarantee the node's own rather than a property of wayfire's
  removal order.
- The destructor detaches through the same `attach(nullptr)`.

Nothing else about filters changes.

## Verified

`test/live.t` (`filters-output`), headless with two outputs. Switching a
headless output off FREES wayfire's output object (an image source of NONE
destroys it), so no nested compositor is needed. Shaders are set on two
windows on HEADLESS-2, and HEADLESS-2 is switched off. Window A's shader is
replaced, and must render on HEADLESS-1 (a pixel in the shader's colour).
Window B's shader is unset WITHOUT a replace, and both must be gone after
the fade. Results:

    stock            segfault at the replace, dragonfly's crash
    patched          ok
    without follow   B's shader never goes (its hook went nowhere): fails
    without backstop ok (see above)

The rig is `/var/tmp/w4/rig.sh` on manifold (cleared after 30 days).

## Upstream

A candidate for soreau/filters, NOT filed: nothing goes to a repo outside
our control for now (the user, 2026-11-18). Drop this patch when the pin
passes an upstream fix.
