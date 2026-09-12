// global-wset.hpp
//
// The "one logical desktop" abstraction, factored out so more than one plugin
// can share it. atlas owns the definition conceptually (see this dir's
// CLAUDE.md): virtual desktop (i, j) is workspace (i, j) on every output
// simultaneously, and the union of all outputs is a single logical screen in
// global layout coordinates. This header is the reusable primitive; the
// directional-focus plugin consumes it today and atlas will #include it when
// it is built.
//
// Nothing here grabs input, lifts limit_region, renders, or declares a
// capability. It is pure query plus a focus helper, safe to call at any time.
//
// All geometry returned is in GLOBAL layout coordinates. Wayfire stores view
// geometry output-local (each output's origin is 0,0 in its own space;
// scene.hpp:415-417), so we translate by the owning output's layout offset
// (get_layout_geometry(), output.hpp:101) -- the same reconciliation scale.cpp
// does at 543-550. Verified against Wayfire v0.10.1 (746bc7e).

#pragma once

#include <optional>
#include <vector>

#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/window-manager.hpp>
#include <wayfire/workspace-set.hpp>

namespace atlas
{
// A mapped toplevel with its geometry placed on the shared logical desktop.
struct placed_view_t
{
    wayfire_toplevel_view view;
    wf::geometry_t box; // global layout coordinates
};

// Translate an output-local rect into global layout coordinates.
inline wf::geometry_t to_global(wf::output_t *wo, wf::geometry_t g)
{
    const auto off = wo->get_layout_geometry();
    g.x += off.x;
    g.y += off.y;
    return g;
}

// Every mapped, non-minimized toplevel on each output's CURRENT workspace,
// with geometry in global coordinates. This is the logical current workspace
// seen as one desktop spanning all physical outputs. Each output is filtered
// by its OWN current workspace, so the result is correct whether or not the
// outputs are kept in lockstep (wsync); if they diverge, each contributes the
// windows on the workspace it is actually showing.
inline std::vector<placed_view_t> current_ws_views()
{
    std::vector<placed_view_t> out;
    for (auto *wo : wf::get_core().output_layout->get_outputs())
    {
        auto wset = wo->wset();
        if (!wset)
        {
            continue;
        }

        // Membership uses the SAME rule as the waybar taskbar, so the bar's
        // window count and this view set never disagree on what is "in" the
        // current workspace. That rule (build/overlays/waybar/taskbar.cpp,
        // Taskbar::bbox_center_on_output): a view is in iff the CENTRE of its
        // bounding box lies inside the output viewport. Both are output-local
        // with the current workspace at the origin, so the test is
        // `centre in [0, w) x [0, h)`. get_bounding_box() is the exact value
        // the IPC publishes as "bbox" (ipc-rules-common.hpp), so the two sides
        // compute the identical number.
        //
        // This is deliberately NOT wf::WSET_CURRENT_WORKSPACE (it counts any
        // OVERLAP, so a maximized window nudged one workspace over pokes a
        // few-pixel sliver over the edge and leaks in as a foreign thumbnail),
        // and NOT get_view_main_workspace (correct, but waybar cannot call it
        // over IPC -- matching waybar's own mechanism keeps them in lockstep
        // wherever the line moves: change it in taskbar.cpp and here together).
        const auto vp = wo->get_relative_geometry(); // {0, 0, w, h}
        for (auto &v : wset->get_views(wf::WSET_MAPPED_ONLY |
            wf::WSET_EXCLUDE_MINIMIZED))
        {
            const auto bb = v->get_bounding_box();
            const int cx = bb.x + bb.width / 2;
            const int cy = bb.y + bb.height / 2;
            if ((cx < vp.x) || (cx >= vp.x + vp.width) ||
                (cy < vp.y) || (cy >= vp.y + vp.height))
            {
                continue;
            }

            out.push_back({v, to_global(wo, v->get_geometry())});
        }
    }

    return out;
}

// Every mapped, non-minimized toplevel on EVERY workspace of every output, in
// global layout coordinates -- the all-workspaces logical desktop, for
// ring_all. An off-current-workspace view's geometry is its off-viewport
// position; that is fine, the ring layout only needs each view's size and then
// assigns it a cell.
inline std::vector<placed_view_t> all_ws_views()
{
    std::vector<placed_view_t> out;
    for (auto *wo : wf::get_core().output_layout->get_outputs())
    {
        auto wset = wo->wset();
        if (!wset)
        {
            continue;
        }

        for (auto &v : wset->get_views(wf::WSET_MAPPED_ONLY |
            wf::WSET_EXCLUDE_MINIMIZED))
        {
            out.push_back({v, to_global(wo, v->get_geometry())});
        }
    }

    return out;
}

// The currently focused toplevel, placed on the logical desktop, if any.
inline std::optional<placed_view_t> focused_view()
{
    auto v = wf::toplevel_cast(wf::get_core().seat->get_active_view());
    if (!v || !v->get_output())
    {
        return {};
    }

    return placed_view_t{v, to_global(v->get_output(), v->get_geometry())};
}

// Move keyboard focus to a view: switch the active OUTPUT if the view is on a
// different one, but never the WORKSPACE. focus_request() would switch the
// workspace too -- it calls focus_raise_view(view, allow_switch_ws=true), whose
// ensure_visible() scrolls to the workspace holding the view's CENTRE
// (window-manager.cpp:158, workspace-set.hpp:153). A WSET_CURRENT_WORKSPACE
// candidate can straddle the edge with its centre on the neighbour, so
// focus_request() teleports one workspace over -- the "occasional jump to
// another workspace" in directional-focus. Call focus_raise_view directly with
// allow_switch_ws=false: focus_output() runs unconditionally so cross-monitor
// focus still works, but the ensure_visible() scroll is skipped. Pass true only
// for an all-workspaces action that means to cross workspaces.
inline void focus(wayfire_toplevel_view view, bool allow_switch_ws = false)
{
    wf::get_core().default_wm->focus_raise_view(view, allow_switch_ws);
}
} // namespace atlas
