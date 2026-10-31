// global-wall.hpp
//
// atlas's workspace-grid renderer: presents the N-output logical desktop as one
// grid of workspace thumbnails. Cell (i, j) is the FULL logical desktop: every
// output's wset at workspace (i, j), composited at its layout offset. The whole
// grid is scaled so the outputs' union shows the complete grid-sized array at
// once, so the grid spans physical output boundaries (a cell divider lands
// mid-panel, by design; see this dir's CLAUDE.md).
//
// It renders on every output at once by living at the scene ROOT (not under an
// output node), so unlike ring it needs no limit_region lift: each output just
// clips the union to its own framebuffer.
//
// Derived from Wayfire's plugins/common/workspace-wall.cpp @ 746bc7e (v0.10.1),
// generalised from a single output to the union of all outputs and stripped of
// the aux-buffer rescale optimisation. Re-verify against /opt/wayfire on bumps.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <wayfire/geometry.hpp>
#include <wayfire/output.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/config/types.hpp>

#include "atlas-anim.hpp"

namespace atlas
{
// Owns the wall render node and its scene-graph membership. Construct with the
// gap between cells and the backdrop colour; start() inserts the node at the
// scene root, stop() (and the destructor) removes it.
class global_wall_t
{
  public:
    global_wall_t(int gap_size, wf::color_t background, int anim_ms);
    ~global_wall_t();

    global_wall_t(const global_wall_t&) = delete;
    global_wall_t& operator =(const global_wall_t&) = delete;

    void start();
    void stop();

    // Run the close animation (the grid zooms back into the current workspace),
    // then invoke on_done once, deferred to an idle so the caller may safely
    // destroy this object from within it. The destructor is the instant
    // teardown for the cancel path (desktop lock).
    void animate_out(std::function<void()> on_done);

    // True once animate_out has begun (a second exit falls back to an instant
    // teardown instead of restarting the close).
    bool closing() const;

    // The workspace cell under a global/layout-space point, or nullopt if the
    // point is on a gap or outside the grid. For click-to-switch.
    std::optional<wf::point_t> cell_at(wf::pointf_t global) const;

    // The topmost mapped toplevel under a global point, paired with the cell it
    // sits in, or nullopt if there is no view there. For starting a drag.
    std::optional<std::pair<wayfire_toplevel_view, wf::point_t>>
    view_at(wf::pointf_t global) const;

    // Where a drop lands: the cell (workspace) whose region is under the point.
    // The preview aims here; a release commits the grabbed window to it.
    std::optional<std::pair<wf::output_t*, wf::point_t>>
    drop_target(wf::pointf_t global) const;

    // Mark a cell as the keyboard selection (a distinct wash over the whole
    // cell), or nullopt to clear.
    void set_selection(std::optional<wf::point_t> cell);

    // Tint the grabbed view blue, "this is the window a drag will move", or
    // nullptr to clear. The blue preview shows the window at its OWN position
    // within whichever workspace is the current target (set_grab_target), and
    // tweens between cells as the target changes. Distinct from the green cell
    // washes (the window is the thing; a cell is the place).
    void set_grabbed(wayfire_toplevel_view view);

    // Aim the drag preview at a workspace cell: the blue window animates to its
    // same position in that cell. No-op if nothing is grabbed or the cell is
    // unchanged. Aiming back at the home cell previews a no-op (nothing moves).
    void set_grab_target(wf::point_t cell);

    // The cell the preview is currently aimed at (what a release commits to).
    wf::point_t grab_target() const;

  private:
    std::shared_ptr<wf::scene::node_t> node;
    int gap_size;
    wf::color_t background;
    int anim_ms;
    anim_driver_t driver;               // open/close zoom
    std::unique_ptr<anim_driver_t> preview;   // drag-preview tween pump
};
}
