// global-ring.hpp
//
// atlas's window overview (ring / ring_all): the current (or every) workspace's
// windows, gathered across all outputs as one logical desktop and shown as a
// grid of scaled thumbnails spanning the union.
//
// Like the switcher, it HAND-PAINTS: each window is rendered into an aux buffer
// and textured onto an opaque backdrop by an overlay node at the scene root.
// So it needs NO limit_region lift -- it occludes the live desktop cleanly
// (backdrop first, then only the arranged thumbnails), spans outputs because
// the node lives at the root, and carries no teardown-critical restore.
//
// This replaces the first-cut ring, which lifted limit_region and put a 2D
// transform on each live view: that left no backdrop (the desktop and other
// outputs bled through the gaps, inconsistently per output) and never occluded
// non-arranged windows. Same spanning-render path as global-wall/global-switch.

#pragma once

#include <functional>
#include <memory>

#include <wayfire/config/types.hpp>

namespace atlas
{
class global_ring_t
{
  public:
    // all_workspaces: false = current workspace (ring), true = every workspace
    // (ring_all). background is the opaque backdrop; gap is the pixel gap
    // between grid cells; anim_ms is the open/close animation duration.
    global_ring_t(bool all_workspaces, wf::color_t background, int gap,
        int anim_ms);
    ~global_ring_t();

    global_ring_t(const global_ring_t&) = delete;
    global_ring_t& operator =(const global_ring_t&) = delete;

    // Collect the windows, insert the overlay, and begin the open animation
    // (each thumbnail slides from its real on-screen position to its grid
    // cell). Returns false if there are none to show (the caller then should
    // not enter the mode).
    bool start();

    // Run the close animation (thumbnails slide back to their real positions),
    // then invoke on_done once, deferred to an idle so the caller may safely
    // destroy this object from within it. Used for the graceful Escape/toggle
    // exit; the destructor is still the instant teardown for the cancel path
    // (desktop lock), which must not wait for an animation.
    void animate_out(std::function<void()> on_done);

    // True once animate_out has begun, so a second exit request can fall back
    // to an instant teardown instead of restarting the close.
    bool closing() const;

  private:
    class impl;
    std::unique_ptr<impl> priv;
};
} // namespace atlas
