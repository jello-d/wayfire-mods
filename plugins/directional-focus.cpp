// directional-focus.cpp
//
// Move keyboard focus to the nearest window in a direction, treating all
// physical outputs as one logical desktop. Only windows on the current
// workspace are candidates; focus never wraps off the end of the layout and
// never crosses to another workspace, but it WILL cross a monitor boundary
// when the nearest window in the chosen direction happens to live on another
// output. That is the whole point: three portrait panels behave like one wide
// desktop for the purpose of "focus the window to my right".
//
// This is the instantaneous, no-grab sibling of the atlas overview plugin. It
// shares atlas's "one logical desktop" abstraction (global-wset.hpp) and
// nothing else: no input grab, no limit_region lift, no rendering, no
// capability, no teardown risk. It is a keybinding that runs a computation and
// changes focus, closer in weight to wsync than to expo.
//
// Verified against Wayfire v0.10.1 (746bc7e). See global-wset.hpp for the
// API citations behind the enumeration and focus calls.

#include <cmath>

#include <wayfire/plugin.hpp>
#include <wayfire/core.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/view.hpp>
#include <wayfire/plugins/ipc/ipc-activator.hpp>
#include <wayfire/util/log.hpp>

#include "global-wset.hpp"

namespace
{
enum class dir_t
{
    left,
    right,
    up,
    down,
};

inline double center_x(const wf::geometry_t &g)
{
    return g.x + g.width / 2.0;
}

inline double center_y(const wf::geometry_t &g)
{
    return g.y + g.height / 2.0;
}
} // namespace

class directional_focus_t : public wf::plugin_interface_t
{
  public:
    // Cross-axis penalty. A candidate offset sideways from the travel
    // direction is scored as if it were perp_weight times further along the
    // direction, so "right" prefers a window roughly level with the source
    // over one that is nearer but far off vertically. 2.0 keeps travel honest
    // without ever grabbing a window that is behind the source.
    wf::option_wrapper_t<double> perp_weight{"directional-focus/perp_weight"};
    wf::option_wrapper_t<bool> debug_log{"directional-focus/debug_log"};

    wf::ipc_activator_t bind_left{"directional-focus/focus_left"};
    wf::ipc_activator_t bind_right{"directional-focus/focus_right"};
    wf::ipc_activator_t bind_up{"directional-focus/focus_up"};
    wf::ipc_activator_t bind_down{"directional-focus/focus_down"};

    void init() override
    {
        bind_left.set_handler([=] (wf::output_t*, wayfire_view)
        {
            return move(dir_t::left);
        });
        bind_right.set_handler([=] (wf::output_t*, wayfire_view)
        {
            return move(dir_t::right);
        });
        bind_up.set_handler([=] (wf::output_t*, wayfire_view)
        {
            return move(dir_t::up);
        });
        bind_down.set_handler([=] (wf::output_t*, wayfire_view)
        {
            return move(dir_t::down);
        });

        LOGI("DIRECTIONAL-FOCUS init: BUILD=", __DATE__, " ", __TIME__);
    }

    // ipc_activator_t removes its binding and IPC method in its destructor, so
    // there is nothing to tear down here.
    void fini() override
    {}

  private:
    // Resolve the direction of travel and the perpendicular for one candidate,
    // measured between rect centers. `along` is the signed distance in the
    // travel direction (positive means the candidate is ahead of the source);
    // `perp` is the sideways center offset.
    static void project(dir_t dir, const wf::geometry_t &src,
        const wf::geometry_t &cand, double &along, double &perp)
    {
        switch (dir)
        {
          case dir_t::right:
            along = center_x(cand) - center_x(src);
            perp  = center_y(cand) - center_y(src);
            break;

          case dir_t::left:
            along = center_x(src) - center_x(cand);
            perp  = center_y(cand) - center_y(src);
            break;

          case dir_t::down:
            along = center_y(cand) - center_y(src);
            perp  = center_x(cand) - center_x(src);
            break;

          case dir_t::up:
            along = center_y(src) - center_y(cand);
            perp  = center_x(cand) - center_x(src);
            break;
        }
    }

    bool move(dir_t dir)
    {
        auto views = atlas::current_ws_views();
        if (views.empty())
        {
            return false;
        }

        // Search origin: the focused window's global rect, or a zero-size rect
        // at the cursor when nothing is focused, so the first press picks the
        // window nearest the pointer in the chosen direction.
        wf::geometry_t src;
        wayfire_toplevel_view current = nullptr;
        if (auto f = atlas::focused_view())
        {
            src     = f->box;
            current = f->view;
        } else
        {
            const auto p = wf::get_core().get_cursor_position();
            src = wf::geometry_t{(int)p.x, (int)p.y, 0, 0};
        }

        const double w = perp_weight;

        const atlas::placed_view_t *best = nullptr;
        double best_score = 0;

        for (auto &pv : views)
        {
            if (pv.view == current)
            {
                continue; // do not "move" onto the focused window itself
            }

            double along, perp;
            project(dir, src, pv.box, along, perp);
            if (along <= 0)
            {
                continue; // not ahead of the source in this direction
            }

            // Squared weighted distance: nearest ahead wins, sideways offset
            // penalized. Monotonic, so comparing the squares is enough.
            const double wp = w * perp;
            const double score = along * along + wp * wp;
            if (!best || (score < best_score))
            {
                best       = &pv;
                best_score = score;
            }
        }

        if (!best)
        {
            // Nothing that way. No wrap, no workspace change: stay put.
            return false;
        }

        if ((bool)debug_log)
        {
            LOGI("DIRECTIONAL-FOCUS: dir=", (int)dir, " -> view at ",
                best->box.x, ",", best->box.y);
        }

        atlas::focus(best->view);
        return true;
    }
};

DECLARE_WAYFIRE_PLUGIN(directional_focus_t)
