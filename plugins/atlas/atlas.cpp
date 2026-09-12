// atlas.cpp
//
// atlas -- present N physical outputs as one logical desktop for overview and
// window switching. See this directory's parent CLAUDE.md for the full design.
//
// STATUS: activation shell. It declares CAPABILITY_MANAGE_COMPOSITOR (core then
// gives mutual exclusion with expo/scale for free), binds wall/ring/ring_all,
// activates across every output with clean rollback, grabs input on each output
// (Escape exits -- this is the escape hatch that makes the limit_region lift
// safe), lifts limit_region behind an RAII guard, and tears all of that down on
// deactivate, on cancel (desktop lock), and on unload.
//
// RING renders now: the current workspace's windows across every output are
// collected via global-wset.hpp and laid out as a grid of scaled thumbnails
// that spans all monitors (the lifted limit_region is what lets one grid cross
// output boundaries). Input is grabbed -- press Escape to exit; selection and
// drag are not wired yet, so it is a modal, look-only overview. WALL (the
// workspace-grid renderer, needs a global workspace-wall) and RING_ALL (all
// workspaces, needs off-workspace views enabled) still only lift-and-grab, and
// there is no open/close animation yet. Teardown stays the design's
// highest-risk item: a lifted limit_region that never restores, with no way
// out, wedges the session (it did once, before the grab existed -- hence
// Escape lands with the lift), so transformers are torn down by re-querying
// LIVE views, never a stored observer_ptr a mid-overview unmap would dangle.
//
// Verified against Wayfire v0.10.1 (746bc7e).

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wayfire/plugin.hpp>
#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-input.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/view.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/plugins/common/input-grab.hpp>
#include <wayfire/plugins/common/util.hpp>
#include <wayfire/plugins/ipc/ipc-activator.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/util.hpp>
#include <wayfire/util/log.hpp>

#include "../global-wset.hpp"
#include "global-wall.hpp"
#include "global-switcher.hpp"
#include "global-ring.hpp"

namespace
{
enum class atlas_mode_t
{
    wall,        // global workspace-grid layout (read-write)
    ring,        // global window layout, current workspace
    ring_all,    // global window layout, all workspaces
    switch_,     // cross-output alt+tab, current workspace
    switch_all,  // cross-output alt+tab, all workspaces
};

// Name of the 2D transformer atlas adds to each view; used to add it and, on
// teardown, to find and remove exactly atlas's transform and no other.
constexpr const char *ATLAS_TRANSFORMER = "atlas";

// Bounding box of every output in global layout coordinates -- the logical
// screen atlas lays its cells out within.
inline wf::geometry_t outputs_union()
{
    bool first = true;
    wf::geometry_t u{0, 0, 0, 0};
    for (auto *wo : wf::get_core().output_layout->get_outputs())
    {
        auto g = wo->get_layout_geometry();
        if (first)
        {
            u = g;
            first = false;
            continue;
        }

        int x1 = std::min(u.x, g.x);
        int y1 = std::min(u.y, g.y);
        int x2 = std::max(u.x + u.width, g.x + g.width);
        int y2 = std::max(u.y + u.height, g.y + g.height);
        u = {x1, y1, x2 - x1, y2 - y1};
    }

    return u;
}

// Split `area` into the smallest near-square grid holding n cells, leaving gap
// px around and between them. One rect per index, row-major.
inline std::vector<wf::geometry_t> grid_cells(size_t n, wf::geometry_t area,
    int gap)
{
    std::vector<wf::geometry_t> cells;
    if (n == 0)
    {
        return cells;
    }

    int cols = (int)std::ceil(std::sqrt((double)n));
    int rows = (int)std::ceil((double)n / cols);
    double cw = (area.width - gap * (cols + 1)) / (double)cols;
    double ch = (area.height - gap * (rows + 1)) / (double)rows;
    for (size_t k = 0; k < n; k++)
    {
        int c = (int)(k % (size_t)cols);
        int r = (int)(k / (size_t)cols);
        cells.push_back({
            area.x + gap + (int)std::lround(c * (cw + gap)),
            area.y + gap + (int)std::lround(r * (ch + gap)),
            (int)std::lround(cw),
            (int)std::lround(ch)});
    }

    return cells;
}
}

// Lift limit_region on every output's scene node so the scene renders across
// output boundaries. src/core/scene.cpp gen_render_instances() returns early
// while a node's limit_region is set and shown_on != that output; nullopt
// disables that clip. Restore the per-output geometry on destruction. THE
// teardown-critical object: its destructor must always run, so the plugin owns
// it in a std::optional it resets on every exit path.
class limit_guard_t
{
  public:
    limit_guard_t()
    {
        auto root = wf::get_core().scene();
        for (auto *wo : wf::get_core().output_layout->get_outputs())
        {
            for (auto& node : wf::collect_output_nodes(root, wo))
            {
                node->limit_region = std::nullopt;
                lifted.push_back({wo, node});
            }
        }

        refresh();
    }

    ~limit_guard_t()
    {
        for (auto& entry : lifted)
        {
            entry.second->limit_region = entry.first->get_layout_geometry();
        }

        refresh();
    }

    limit_guard_t(const limit_guard_t&) = delete;
    limit_guard_t& operator =(const limit_guard_t&) = delete;

  private:
    std::vector<std::pair<wf::output_t*,
        std::shared_ptr<wf::scene::output_node_t>>> lifted;

    void refresh()
    {
        wf::scene::update(wf::get_core().scene(),
            wf::scene::update_flag::INPUT_STATE);
        for (auto *wo : wf::get_core().output_layout->get_outputs())
        {
            wo->render->damage_whole();
        }
    }
};

class atlas_t : public wf::plugin_interface_t,
    public wf::keyboard_interaction_t,
    public wf::pointer_interaction_t
{
  public:
    wf::option_wrapper_t<bool> debug_log{"atlas/debug_log"};
    wf::option_wrapper_t<int> gap_size{"atlas/gap_size"};
    wf::option_wrapper_t<int> anim_duration{"atlas/anim_duration"};
    wf::option_wrapper_t<wf::color_t> background{"atlas/background"};
    wf::option_wrapper_t<double> inactive_brightness{
        "atlas/inactive_brightness"};

    wf::ipc_activator_t bind_wall{"atlas/wall"};
    wf::ipc_activator_t bind_ring{"atlas/ring"};
    wf::ipc_activator_t bind_ring_all{"atlas/ring_all"};
    wf::ipc_activator_t bind_switch{"atlas/switch"};
    wf::ipc_activator_t bind_switch_all{"atlas/switch_all"};

    void init() override
    {
        grab.name = "atlas";
        grab.capabilities = wf::CAPABILITY_MANAGE_COMPOSITOR;
        grab.cancel = [this] () { deactivate(); };

        bind_wall.set_handler([this] (wf::output_t*, wayfire_view)
        {
            return toggle(atlas_mode_t::wall);
        });
        bind_ring.set_handler([this] (wf::output_t*, wayfire_view)
        {
            return toggle(atlas_mode_t::ring);
        });
        bind_ring_all.set_handler([this] (wf::output_t*, wayfire_view)
        {
            return toggle(atlas_mode_t::ring_all);
        });
        bind_switch.set_handler([this] (wf::output_t*, wayfire_view)
        {
            return toggle(atlas_mode_t::switch_);
        });
        bind_switch_all.set_handler([this] (wf::output_t*, wayfire_view)
        {
            return toggle(atlas_mode_t::switch_all);
        });

        LOGI("ATLAS init: BUILD=", __DATE__, " ", __TIME__);
    }

    void fini() override
    {
        deactivate();
    }

    // The grab routes keyboard here regardless of the (cross-output, and thus
    // broken while lifted) pointer hit-testing, so Escape always gets us out.
    void handle_keyboard_key(wf::seat_t*, wlr_keyboard_key_event ev) override
    {
        // switch/switch_all are a held-modifier grab (Tab cycles, releasing the
        // activating modifier commits), so they need release events too.
        if (active && ((current == atlas_mode_t::switch_) ||
            (current == atlas_mode_t::switch_all)))
        {
            handle_switch_key(ev);
            return;
        }

        if (ev.state != WLR_KEY_PRESSED)
        {
            return;
        }

        if (ev.keycode == KEY_ESC)
        {
            exit_mode();   // ring animates closed; wall tears down instantly
            return;
        }

        // Arrow keys move the selection, Enter jumps there. Wall only.
        if ((current != atlas_mode_t::wall) || !wall)
        {
            return;
        }

        auto grid = wall_grid();
        switch (ev.keycode)
        {
          case KEY_LEFT:  move_sel(-1, 0, grid); break;
          case KEY_RIGHT: move_sel(1, 0, grid);  break;
          case KEY_UP:    move_sel(0, -1, grid); break;
          case KEY_DOWN:  move_sel(0, 1, grid);  break;

          case KEY_ENTER:
          case KEY_KPENTER:
            switch_workspace_all(wall_sel);
            deactivate();
            break;

          default:
            break;
        }
    }

    // Wall is interactive: left-click a cell to switch to that workspace and
    // exit; press-drag-release a window to move it to another workspace and
    // stay open. Ring/ring_all remain modal (Escape only).
    void handle_pointer_button(const wlr_pointer_button_event& ev) override
    {
        if (!active || (current != atlas_mode_t::wall) || !wall)
        {
            return;
        }

        if (ev.button != BTN_LEFT)
        {
            return;
        }

        auto gc = wf::get_core().get_cursor_position();
        if (ev.state == WL_POINTER_BUTTON_STATE_PRESSED)
        {
            wall_btn_down = true;
            wall_dragging = false;
            wall_press    = gc;
            drag_view     = nullptr;
            if (auto hit = wall->view_at(gc))
            {
                drag_view = hit->first;
            }

            wall->set_grabbed(drag_view);   // blue: this window will move
            return;
        }

        wall_btn_down = false;
        if (wall_dragging)
        {
            drop_drag();                 // commit while the blue preview shows
            wall->set_grabbed(nullptr);  // then clear it: the window has landed
            return;   // a drag leaves the overview open
        }

        // A plain click switches to the clicked workspace and exits.
        wall->set_grabbed(nullptr);
        drag_view = nullptr;
        if (auto cell = wall->cell_at(gc))
        {
            switch_workspace_all(*cell);
            deactivate();
        }
    }

    void handle_pointer_motion(wf::pointf_t, uint32_t) override
    {
        if (!active || (current != atlas_mode_t::wall) || !wall_btn_down)
        {
            return;
        }

        auto gc = wf::get_core().get_cursor_position();
        if (!wall_dragging && drag_view)
        {
            double dx = gc.x - wall_press.x;
            double dy = gc.y - wall_press.y;
            if (dx * dx + dy * dy >= 100.0)   // ~10px: press vs drag threshold
            {
                wall_dragging = true;
            }
        }

        if (wall_dragging && wall)
        {
            // Aim the blue preview at the workspace under the cursor; it tweens
            // there. Only the CELL matters (the window keeps its own output and
            // its own position), so a gap between cells just leaves it put.
            if (auto tgt = wall->drop_target(gc))
            {
                wall->set_grab_target(tgt->second);
            }
        }
    }

  private:
    wf::plugin_activation_data_t grab;
    bool active = false;
    atlas_mode_t current = atlas_mode_t::wall;
    std::optional<limit_guard_t> guard;
    std::unique_ptr<atlas::global_wall_t> wall;
    std::unique_ptr<atlas::global_switcher_t> switcher;
    std::unique_ptr<atlas::global_ring_t> ring;
    wf::wl_idle_call close_idle;   // defers ring/wall teardown out of its hook
    uint32_t activating_mods = 0;   // modifiers held when switch activated
    bool shift_held = false;        // live Shift state -> cycle direction
    bool wall_btn_down = false;
    bool wall_dragging = false;
    wf::pointf_t wall_press{0, 0};
    wayfire_toplevel_view drag_view = nullptr;
    wf::point_t wall_sel{0, 0};   // keyboard-selected cell in wall mode
    std::vector<wf::output_t*> held;
    std::vector<std::unique_ptr<wf::input_grab_t>> input_grabs;

    // Release everything activate() grabbed (input grabs + plugin activations),
    // for the empty-overview bail paths that must not leave the box grabbed.
    void unwind_activation()
    {
        for (auto& g : input_grabs)
        {
            g->ungrab_input();
        }

        input_grabs.clear();
        for (auto *wo : held)
        {
            wo->deactivate_plugin(&grab);
        }

        held.clear();
    }

    // The graceful exit. The ring and wall overviews run their close animation
    // (thumbnails slide home / the grid zooms back into the current workspace)
    // and tear down only when it finishes -- deferred to an idle because the
    // overlay signals completion from inside its own render hook, where
    // destroying it would be unsafe (rem_effect on the running hook). switch,
    // and a second exit request while a close is already running, tear down
    // instantly. The cancel path (grab.cancel, desktop lock) calls deactivate()
    // directly and so never animates.
    void exit_mode()
    {
        auto defer_teardown = [this] ()
        {
            close_idle.run_once([this] () { deactivate(); });
        };

        if (active && ring && !ring->closing() &&
            ((current == atlas_mode_t::ring) ||
                (current == atlas_mode_t::ring_all)))
        {
            ring->animate_out(defer_teardown);
            return;
        }

        if (active && wall && !wall->closing() &&
            (current == atlas_mode_t::wall))
        {
            wall->animate_out(defer_teardown);
            return;
        }

        deactivate();
    }

    bool toggle(atlas_mode_t mode)
    {
        if (active)
        {
            exit_mode();
            return true;
        }

        return activate(mode);
    }

    // atlas is global: every output or none. Pre-check so a busy output
    // (expo/scale already open) aborts before we grab anything, then activate
    // with rollback in case the state changed underneath us.
    bool activate(atlas_mode_t mode)
    {
        auto outputs = wf::get_core().output_layout->get_outputs();
        for (auto *wo : outputs)
        {
            if (!wo->can_activate_plugin(&grab))
            {
                if ((bool)debug_log)
                {
                    LOGI("ATLAS: ", wo->handle->name, " busy; not activating");
                }

                return false;
            }
        }

        for (auto *wo : outputs)
        {
            if (!wo->activate_plugin(&grab))
            {
                for (auto *done : held)
                {
                    done->deactivate_plugin(&grab);
                }

                held.clear();
                return false;
            }

            held.push_back(wo);
        }

        // Grab input on every held output BEFORE lifting limits, so keyboard
        // (Escape) is captured even once pointer hit-testing goes cross-output.
        for (auto *wo : held)
        {
            auto g = std::make_unique<wf::input_grab_t>("atlas", wo, this, this,
                nullptr);
            g->grab_input(wf::scene::layer::OVERLAY);
            input_grabs.push_back(std::move(g));
        }

        current = mode;
        if ((mode == atlas_mode_t::ring) || (mode == atlas_mode_t::ring_all))
        {
            // ring/ring_all hand-paint their thumbnails onto a backdrop at the
            // scene root, so -- like wall and switch -- they need NO
            // limit_region lift and occlude the live desktop cleanly. ring is
            // the current workspace only; ring_all is every workspace.
            ring = std::make_unique<atlas::global_ring_t>(
                mode == atlas_mode_t::ring_all, (wf::color_t)background,
                (int)gap_size, (int)anim_duration);
            if (!ring->start())
            {
                ring.reset();
                unwind_activation();   // no windows: release grab + activation
                return false;
            }
        }
        else if (mode == atlas_mode_t::wall)
        {
            // wall renders at the scene root, so it spans outputs without any
            // lift -- just insert the node.
            wall = std::make_unique<atlas::global_wall_t>((int)gap_size,
                (wf::color_t)background, (int)anim_duration);
            wall->start();
            wall_sel = wall_current_ws();
            wall->set_selection(wall_sel);
        }
        else   // switch_ / switch_all
        {
            // switch hand-paints its own overlay across outputs, so it needs no
            // limit_region lift. Record the modifiers held at activation: the
            // switch ends when one of them is released (classic alt+tab).
            switcher = std::make_unique<atlas::global_switcher_t>(
                mode == atlas_mode_t::switch_all, (wf::color_t)background,
                (double)inactive_brightness);
            if (!switcher->start())
            {
                // Nothing to switch between: unwind the grab and activation.
                switcher.reset();
                unwind_activation();
                return false;
            }

            activating_mods = wf::get_core().seat->get_keyboard_modifiers();
            shift_held = false;
        }

        active = true;
        LOGI("ATLAS: activated mode=", (int)mode, " across ", held.size(),
            " output(s) -- Escape to exit");

        // TODO(render): the open/close animation is still the missing polish.
        return true;
    }

    void deactivate()
    {
        if (!active)
        {
            return;
        }

        wall_btn_down = false;
        wall_dragging = false;
        drag_view     = nullptr;
        switcher.reset();      // remove the switcher overlay node (dtor stops)
        ring.reset();          // remove the ring overlay node (dtor stops)
        wall.reset();          // remove the wall node (destructor stops it)
        clear_transformers();  // no-op now: ring hand-paints, adds none
        guard.reset();         // no-op now: no mode lifts limit_region
        for (auto& g : input_grabs)
        {
            g->ungrab_input();
        }

        input_grabs.clear();
        for (auto *wo : held)
        {
            wo->deactivate_plugin(&grab);
        }

        held.clear();
        active = false;
        LOGI("ATLAS: deactivated");
    }

    // Held-grab keys for switch/switch_all. Tab cycles (Shift reverses), Escape
    // cancels without committing, and releasing the activating modifier commits
    // focus to the centre window. Shift is tracked by keycode so no wlroots
    // modifier constant is needed.
    void handle_switch_key(wlr_keyboard_key_event ev)
    {
        bool pressed = (ev.state == WLR_KEY_PRESSED);
        if ((ev.keycode == KEY_LEFTSHIFT) || (ev.keycode == KEY_RIGHTSHIFT))
        {
            shift_held = pressed;
            return;
        }

        if (pressed)
        {
            if (ev.keycode == KEY_ESC)
            {
                deactivate();   // cancel: no focus commit
            }
            else if ((ev.keycode == KEY_TAB) && switcher)
            {
                switcher->cycle(shift_held ? 1 : -1);
            }

            return;
        }

        // Release: the activating modifier going up ends the switch, commits.
        auto mod = wf::get_core().seat->modifier_from_keycode(ev.keycode);
        if (mod & activating_mods)
        {
            commit_switch();
        }
    }

    void commit_switch()
    {
        // Grab the target before teardown (deactivate destroys the switcher),
        // then focus it once the overlay is gone.
        auto v = switcher ? switcher->current_view() : nullptr;
        deactivate();
        if (v && v->is_mapped())
        {
            atlas::focus(v);
        }
    }

    // Lay the current workspace's windows out as a grid of scaled thumbnails
    // spanning all outputs. Adds a named 2D transformer per view; the view's
    // transform manager owns it, so an unmap frees it with the view and no
    // pointer is retained to dangle. Call only with limits already lifted.
    void render_windows(const std::vector<atlas::placed_view_t> &placed)
    {
        if (placed.empty())
        {
            return;
        }

        auto cells = grid_cells(placed.size(), outputs_union(),
            std::max(0, (int)gap_size));
        for (size_t k = 0; k < placed.size(); k++)
        {
            place_view(placed[k].view, placed[k].box, cells[k]);
        }

        for (auto *wo : wf::get_core().output_layout->get_outputs())
        {
            wo->render->damage_whole();
        }

        if ((bool)debug_log)
        {
            auto u = outputs_union();
            LOGI("ATLAS ring: ", placed.size(), " window(s); union=", u.x, ",",
                u.y, " ", u.width, "x", u.height);
            for (size_t k = 0; k < placed.size(); k++)
            {
                auto &b = placed[k].box;
                auto &c = cells[k];
                LOGI("  ring[", k, "] src=", b.x, ",", b.y, " ", b.width, "x",
                    b.height, " -> cell=", c.x, ",", c.y, " ", c.width, "x",
                    c.height);
            }
        }
    }

    // Map a view (currently at global box `src`) into `cell`, preserving aspect
    // and centering. view_2d scales about the view's bbox centre then
    // translates (scale.cpp), so translation = cell centre - view centre.
    void place_view(wayfire_toplevel_view view, wf::geometry_t src,
        wf::geometry_t cell)
    {
        if ((src.width <= 0) || (src.height <= 0))
        {
            return;
        }

        double s = std::min((double)cell.width / src.width,
            (double)cell.height / src.height);
        auto tr =
            wf::ensure_named_transformer<wf::scene::view_2d_transformer_t>(
                view, wf::TRANSFORMER_2D + 1, ATLAS_TRANSFORMER, view);
        tr->scale_x = tr->scale_y = (float)s;
        tr->translation_x = (float)((cell.x + cell.width / 2.0) -
            (src.x + src.width / 2.0));
        tr->translation_y = (float)((cell.y + cell.height / 2.0) -
            (src.y + src.height / 2.0));
    }

    // Remove atlas's transformer from every LIVE view still carrying it. This
    // re-queries the current views rather than trusting a stored list: a view
    // that unmapped mid-overview is simply absent (its transformer died with
    // it), so nothing dangles and nothing leaks.
    // Set EVERY output's wset to the same workspace -- atlas's global-desktop
    // switch (all outputs move together, the definition atlas owns). This is
    // the deliberate counter to per-output wset drift, not a victim of it.
    void switch_workspace_all(wf::point_t ws)
    {
        for (auto *wo : wf::get_core().output_layout->get_outputs())
        {
            if (wo->wset())
            {
                wo->wset()->set_workspace(ws);
            }
        }
    }

    // The active output's workspace grid and current workspace, for keyboard
    // navigation (all outputs share the same grid; atlas moves them together).
    wf::dimensions_t wall_grid()
    {
        auto *o = wf::get_core().seat->get_active_output();
        return (o && o->wset()) ? o->wset()->get_workspace_grid_size()
                                : wf::dimensions_t{1, 1};
    }

    wf::point_t wall_current_ws()
    {
        auto *o = wf::get_core().seat->get_active_output();
        return (o && o->wset()) ? o->wset()->get_current_workspace()
                                : wf::point_t{0, 0};
    }

    void move_sel(int dx, int dy, wf::dimensions_t grid)
    {
        wall_sel.x = std::clamp(wall_sel.x + dx, 0, grid.width - 1);
        wall_sel.y = std::clamp(wall_sel.y + dy, 0, grid.height - 1);
        if (wall)
        {
            wall->set_selection(wall_sel);
        }
    }

    // Finish a drag: move the grabbed view to the workspace the preview aims
    // at (grab_target) on its OWN output -- a view keeps its output, only its
    // workspace changes. A no-op when the aim is back on the home workspace, or
    // the view vanished mid-drag; either way the overview stays open.
    void drop_drag()
    {
        if (wall && drag_view && drag_view->is_mapped() &&
            drag_view->get_output())
        {
            auto cell = wall->grab_target();
            if (cell != wall_current_ws())   // home = nothing to do
            {
                auto *out  = drag_view->get_output();
                auto edges = drag_view->pending_tiled_edges();
                if ((bool)debug_log)
                {
                    LOGI("ATLAS drop: ", out->handle->name, " ws ",
                        cell.x, ",", cell.y, " tiled=", edges);
                }

                if (edges)
                {
                    wf::get_core().default_wm->tile_request(drag_view, edges,
                        cell);
                }
                else if (auto ws = out->wset())
                {
                    ws->move_to_workspace(drag_view, cell);
                }

                damage_all();
            }
        }
        else if ((bool)debug_log)
        {
            LOGI("ATLAS drop: no draggable view (grab missed a window)");
        }

        wall_dragging = false;
        drag_view = nullptr;
    }

    void damage_all()
    {
        for (auto *wo : wf::get_core().output_layout->get_outputs())
        {
            wo->render->damage_whole();
        }
    }

    void clear_transformers()
    {
        // all_ws_views is a superset of current_ws_views, so this teardown
        // covers ring (current workspace) and ring_all (every workspace); a
        // view without the atlas transformer is simply skipped.
        int removed = 0;
        int total = 0;
        for (auto &p : atlas::all_ws_views())
        {
            auto node = p.view->get_transformed_node();
            if (node->get_transformer(ATLAS_TRANSFORMER))
            {
                node->rem_transformer<wf::scene::view_2d_transformer_t>(
                    ATLAS_TRANSFORMER);
                removed++;
            }

            if ((bool)debug_log)
            {
                auto &b = p.box;
                LOGI("  teardown[", total, "] box=", b.x, ",", b.y, " ",
                    b.width, "x", b.height);
            }

            total++;
        }

        if ((bool)debug_log)
        {
            LOGI("ATLAS teardown: removed ", removed, " of ", total,
                " view transformer(s)");
        }
    }
};

DECLARE_WAYFIRE_PLUGIN(atlas_t)
