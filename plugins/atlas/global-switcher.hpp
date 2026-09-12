// global-switcher.hpp
//
// atlas's cross-output alt+tab: the current (or every) workspace's windows,
// gathered across all outputs as one logical desktop and shown as a 3-slot MRU
// ring -- previous / current / next -- that cycles on Tab and commits focus on
// release. See this directory's parent CLAUDE.md ("The switcher").
//
// Unlike ring/ring_all it does NOT lift limit_region. It owns an overlay node
// at the scene root that hand-paints each shown window: the window is rendered
// into an aux buffer, then textured into its slot in global layout coordinates
// -- the same spanning-render path global-wall uses, where each output clips
// the union to its own framebuffer. So the overview crosses outputs with no
// per-output scene clip to lift and no teardown-critical restore.
//
// FIRST CUT: flat scaled thumbnails that snap between slots. The 3D Z-ring
// perspective (a view_3d_transformer Y-rotation) and the open/close + slot
// animation are deferred chrome (see the parent CLAUDE.md). MRU order is
// wf::get_focus_timestamp; there is no focus tracker of our own.

#pragma once

#include <memory>

#include <wayfire/config/types.hpp>
#include <wayfire/toplevel-view.hpp>

namespace atlas
{
class global_switcher_t
{
  public:
    // all_workspaces: false = current workspace (switch), true = every
    // workspace (switch_all). background is painted behind the ring;
    // inactive_brightness dims the non-centre (wing) slots.
    global_switcher_t(bool all_workspaces, wf::color_t background,
        double inactive_brightness);
    ~global_switcher_t();

    global_switcher_t(const global_switcher_t&) = delete;
    global_switcher_t& operator =(const global_switcher_t&) = delete;

    // Collect the MRU-ordered windows and insert the overlay. Returns false if
    // there is nothing to show (the caller then must not enter switch mode).
    bool start();

    // Move the ring cursor: dir<0 towards the more-recent window, dir>0 towards
    // the less-recent. Wraps. No-op with fewer than two windows.
    void cycle(int dir);

    // The window under the cursor (centre slot), for the focus commit, or null.
    wayfire_toplevel_view current_view() const;

  private:
    class impl;
    std::unique_ptr<impl> priv;
};
} // namespace atlas
