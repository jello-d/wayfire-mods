# plugins-extra-annotate-grid.patch

Keep annotate's per-workspace overlays in step with the CURRENT workspace
grid, so the grid can grow or shrink while wayfire runs. One file
(`src/annotate.cpp` in wayfire-plugins-extra). The first patch on
plugins-extra, which used to be built clean.

## Why

dragonfly, 2026-11-18 (hwdp W1, `_wayfire-mods-requests-hwdp.md`): the
user grew the laptop desk from 3x3 to 4x3, apply-wayfire-runtime pushed
`core/vwidth` over IPC, and the first move into the new fourth column
aborted wayfire, twice in a minute:

    std::vector<...>::operator[]  Assertion '__n < this->size()' failed
    wayfire_annotate_screen::viewport_changed  <- edge_flipping_t::handle_motion

`init()` built one overlay per workspace of the grid AS IT WAS, and
`viewport_changed` looped over the grid AS IT IS, indexing past the table
on any grid bigger than the one at init. Nothing listened for
`workspace_grid_changed_signal`. A shrink was silently safe (the loop
covered less than the table), which is why docking (3x3 to 2x2) never hit
it. And it re-arms at every login, not only on a live change: tackup's
wayfire.ini leaves vwidth/vheight at the default 3x3 and pushes the desk's
grid after annotate has loaded, so any desk larger than 3x3 is a crash
waiting at its first visit to the new row or column.

## What it does

`sync_grid()` makes the table exactly the current grid: a cell that
survives keeps its drawing, a removed cell is destroyed (its drawing with
it), a new cell starts empty, positioned against the current workspace. It
runs on `workspace_grid_changed_signal` and before EVERY use of the table
(the three overlay getters and `viewport_changed`), because a workspace
set swapped onto the output brings its own grid with no signal on the old
set. `viewport_changed` also loops over the table it has rather than the
grid it reads, so the two cannot disagree. `fini()` shares the node
teardown with the resize.

## Verified

Headless, /opt/wayfire with only annotate swapped (`/var/tmp/w1/rig.sh`):
grow to 4x3 and visit (3,0), shrink to 2x2 and visit (1,1), grow to 5x4 and
visit (4,3), back to 3x3. Stock aborts at the first step with dragonfly's
exact assertion; patched survives all four. `test/live.t` (`annotate-grid`)
does the same against the built prefix.

NOT tested: that a surviving cell's drawing is kept across a resize. Drawing
needs a pointer button binding the headless rig does not drive; the code
keeps those cells' nodes untouched, which is read, not measured.

## Upstream

A candidate for WayfireWM/wayfire-plugins-extra, NOT filed: nothing goes to
a repo outside our control for now (the user, 2026-11-18). The reproduction
above is the report, should that change. Drop this patch when the pin passes
an upstream fix.
