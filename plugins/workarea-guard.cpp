/**
 * workarea-guard.cpp - keep floating windows out of the panel "DMZ".
 *
 * waybar reserves an exclusive zone top and bottom, so the compositor's
 * workarea is inset from the full output. Tiled and maximized views already
 * respect that inset; FLOATING views do not: the `place` plugin skips
 * self-positioning clients (xwayland/wine set startup-x/y), never clamps a
 * window taller than the workarea, and nothing stops a drag from parking a
 * window half under a bar. This plugin adds the missing floor.
 *
 * THE RULE (single, symmetric, config-less):
 *
 *   Let WA = the workarea (the output minus the bars), and let the view's SOLID
 *   FRAME be its geometry minus the decoration shadow (see below). Then:
 *
 *     clamp  iff  the SOLID frame's CENTRE lies inside WA  AND  the solid frame
 *     pokes into a bar zone, and the clamp nudges it VERTICALLY, minimally, so
 *     the solid edge sits flush with the bar (the shadow bleeds under it).
 *
 *   "Centre inside WA" means the window is predominantly in the visible area
 *   between the bars, so a bar overlap is an accident -> snap it out. A window
 *   whose centre has left WA is a deliberate placement -> leave it: shoved half
 *   off the side to make room (centre past the left/right edge), or pushed
 *   mostly onto the neighbour workspace (centre up in the top bar / down in the
 *   bottom bar). New windows that spawn under a bar snap out too.
 *
 *   Using the workarea CENTRE, not full containment in the workspace, is
 *   deliberate: a window tucked under a bar often pokes a few px past the SAME
 *   screen edge (its bottom runs just off-screen), and a full-containment test
 *   wrongly reads that as "off the edge, hands off". The centre test keys on
 *   where the BULK of the window is, so a grazing tuck still snaps while a
 *   half-off make-room gesture does not. The bars set WA, so this needs no
 *   thresholds and adapts to any bar layout (side bars included).
 *
 *   SOLID FRAME, not the shadow, not the content. A decorator draws a drop
 *   shadow around the window and (pixdecor) records its thickness as a per-view
 *   margin, expanding the view geometry to include it, so get_geometry() is
 *   the shadow's outer edge, a shadow-width off the real window, and clamping
 *   that leaves an unusable gap. We strip JUST the shadow margin (read live off
 *   the view, see wf_shadow_margin_t) to get the SOLID decorator edge (titlebar
 *   + border + content) and bound THAT: the solid edge lands flush and only the
 *   shadow bleeds under the bar (a higher layer, so hidden). Stripping the FULL
 *   decoration margin instead would land on the content and tuck the titlebar/
 *   border under the bar; an undecorated view publishes no shadow margin, so
 *   its frame (already the content) is bounded as-is. Config-less: the margin
 *   is read from live view state, never a decorator config.
 *
 * CONFIG-LESS BY DESIGN. There is nothing to tune: the boundary is the live
 * workarea (output->workarea->get_workarea()), read fresh every time, so the
 * DMZ auto-tracks whatever the bars actually reserve: change the waybar
 * height, add or drop a bar, and this follows with no edit. Hardcoding or
 * configuring a bar height would be a second copy of a fact the compositor
 * already owns, and it would drift. To disable, unload the plugin.
 *
 * WHY IT DOES NOT FIGHT INTERACTION. `move` renders a drag through a
 * transformer and commits the real geometry once, at drop, so
 * view-geometry-changed fires a single time per drag, at release, and clamping
 * there is friction (snap out on release), never a rubber-band mid-drag.
 * `resize` instead commits geometry continuously, so we skip size-changing
 * events and only act on pure moves; a window left under a bar by a resize is
 * corrected on its next move, and fresh spawns are caught at map. edge-flipping
 * is untouched: it triggers on the POINTER hitting a screen edge, never on
 * window geometry, so dragging a window to an edge still flips workspaces and
 * carries the window along: this only acts on what is LEFT under a bar.
 */

#include <wayfire/plugin.hpp>
#include <wayfire/core.hpp>
#include <wayfire/object.hpp>
#include <wayfire/view.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/toplevel.hpp>
#include <wayfire/output.hpp>
#include <wayfire/workarea.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/util/log.hpp>

// pixdecor's shadow data (the vendored type) lives in pixdecor-shadow.hpp,
// shared with enclave-mark.
#include "pixdecor-shadow.hpp"

class wayfire_workarea_guard : public wf::plugin_interface_t
{
    // Reentrancy guard: our set_geometry() re-emits view-geometry-changed. The
    // rule is idempotent (a cleared view is a no-op on the next pass), so this
    // is belt-and-suspenders against any synchronous echo, not the primary
    // convergence guarantee.
    bool applying = false;

    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped =
        [=] (wf::view_mapped_signal *ev)
    {
        // A fresh window's initial placement: catches the spawn-under case,
        // including self-positioning clients the `place` plugin skips.
        clamp(wf::toplevel_cast(ev->view));
    };

    wf::signal::connection_t<wf::view_geometry_changed_signal> on_geometry =
        [=] (wf::view_geometry_changed_signal *ev)
    {
        auto view = ev->view;
        if (!view)
        {
            return;
        }

        // Only pure moves. A size change means an in-progress resize (or a
        // tile/untile), which commits geometry live: clamping it would fight
        // the gesture. See the header note on interaction.
        auto g = view->get_geometry();
        if ((g.width != ev->old_geometry.width) ||
            (g.height != ev->old_geometry.height))
        {
            return;
        }

        clamp(view);
    };

    // A view we should manage: a real, mapped, free-floating toplevel. Skip
    // children/dialogs (they track their parent), tiled views (the tiler owns
    // their geometry and already honours the workarea), fullscreen, and sticky
    // views (shown on every workspace: a deliberate, placed thing).
    bool manages(wayfire_toplevel_view view)
    {
        return view && view->is_mapped() && view->get_output() &&
            !view->parent && !view->sticky &&
            !view->pending_fullscreen() && !view->pending_tiled_edges();
    }

    void clamp(wayfire_toplevel_view view)
    {
        if (applying || !manages(view))
        {
            return;
        }

        auto output = view->get_output();
        auto wa = output->workarea->get_workarea();   // the output minus bars
        auto g  = view->get_geometry();               // frame incl. shadow

        // Strip ONLY the drop shadow, keeping the titlebar + border, so we
        // bound the SOLID decorator edge to the bar. Stripping the full
        // decoration margin instead lands on the content and tucks the
        // titlebar/border under the bar; stripping nothing leaves a shadow gap.
        // pixdecor publishes its shadow thickness as a per-view custom_data,
        // read live -> config-less. Absent (undecorated view, or a decorator
        // that does not publish it) -> zero margin -> we bound the frame, which
        // for an undecorated view already IS the content.
        auto s = wf::shrink_geometry_by_margins(g, pixdecor_shadow(view));
        if ((s.width <= 0) || (s.height <= 0))
        {
            return;   // degenerate strip (e.g. mid shade-animation): leave it
        }

        // Act only when the SOLID frame's CENTRE lies inside the workarea: the
        // window is predominantly in the visible area, so a bar overlap is
        // accidental. A centre outside WA is a deliberate placement (shoved
        // half off the side to make room, or pushed mostly onto the neighbour
        // workspace): hands off. Centre, not full containment, so a tuck that
        // pokes a few px past the same screen edge still counts.
        int cx = s.x + s.width / 2;
        int cy = s.y + s.height / 2;
        bool center_in_wa =
            (cx >= wa.x) && (cx < wa.x + wa.width) &&
            (cy >= wa.y) && (cy < wa.y + wa.height);
        if (!center_in_wa)
        {
            return;
        }

        // Minimal vertical shift to bring the SOLID frame into the workarea;
        // the stripped shadow bleeds under the bar. Applied to the real frame
        // geometry g (the solid frame is g minus fixed margins, so both move by
        // the same dy).
        int dy = 0;
        if (s.y < wa.y)
        {
            // Solid top under the top bar: push down flush beneath it
            // (titlebar visible, shadow hidden under the bar). Works for a
            // too-tall window too: top clears, bottom overflow accepted.
            dy = wa.y - s.y;
        }
        else if ((s.height <= wa.height) &&
                 (s.y + s.height > wa.y + wa.height))
        {
            // Solid bottom under the bottom bar: push up flush. A too-tall
            // window is intentionally NOT pushed here: nothing gains it.
            dy = (wa.y + wa.height) - (s.y + s.height);
        }

        if (dy == 0)
        {
            return;                                   // already clear: no-op
        }

        applying = true;
        view->set_geometry({g.x, g.y + dy, g.width, g.height});
        applying = false;
        LOGD("workarea-guard: snapped floating view by dy ", dy);
    }

  public:
    void init() override
    {
        wf::get_core().connect(&on_view_mapped);
        wf::get_core().connect(&on_geometry);
        LOGI("workarea-guard: loaded");
    }

    void fini() override
    {
        on_view_mapped.disconnect();
        on_geometry.disconnect();
        LOGI("workarea-guard: unloaded");
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_workarea_guard);
