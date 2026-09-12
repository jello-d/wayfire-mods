// global-switcher.cpp -- see global-switcher.hpp. The spanning aux-buffer +
// add_texture render path is derived from atlas's own global-wall.cpp, which in
// turn follows Wayfire's plugins/common/workspace-wall.cpp @ 746bc7e (v0.10.1).
// The MRU-ring arrangement is derived from plugins/single_plugins/switcher.cpp
// (same tag), flattened (no view_3d_transformer) and static (no animation) for
// this first cut.

#include "global-switcher.hpp"
#include "../global-wset.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/region.hpp>
#include <wayfire/render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/view-helpers.hpp>
#include <wayfire/plugins/common/util.hpp>

namespace atlas
{
namespace
{
// Bounding box of every output in layout coordinates -- the logical desktop.
wf::geometry_t outputs_union()
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

// Fit a vw x vh box into an mw x mh box centred at (cx, cy), preserving aspect
// and never upscaling past 1x. Result is in the same coordinate space as
// (cx, cy).
wf::geometry_t fit_centered(int vw, int vh, int cx, int cy, int mw, int mh)
{
    if ((vw <= 0) || (vh <= 0))
    {
        return {cx, cy, 0, 0};
    }

    double s = std::min({(double)mw / vw, (double)mh / vh, 1.0});
    int w = (int)std::lround(vw * s);
    int h = (int)std::lround(vh * s);
    return {cx - w / 2, cy - h / 2, w, h};
}

// One visible ring slot: a window rendered into its own buffer and textured
// into `box` (global layout coordinates). `src` is the window's content bbox in
// its own output-local space -- the buffer size and the render source.
struct slot_t
{
    wayfire_toplevel_view view;
    wf::output_t *output;
    wf::geometry_t src;
    wf::geometry_t box;
    bool dim;   // a wing (non-centre) slot -> darken by inactive_brightness
    wf::auxilliary_buffer_t buffer;
    bool need_render = true;
};

class switcher_node_t : public wf::scene::node_t
{
  public:
    wf::color_t background;
    double inactive_brightness;
    wf::geometry_t u{0, 0, 0, 0};
    std::vector<slot_t> slots;

    switcher_node_t(wf::color_t bg, double ib) :
        node_t(false), background(bg), inactive_brightness(ib)
    {
        u = outputs_union();
    }

    // Spans the whole logical desktop, so the overlay suppresses the normal
    // desktop on every output (each output clips the union to its framebuffer).
    wf::geometry_t get_bounding_box() override
    {
        return u;
    }

    std::string stringify() const override
    {
        return "atlas-global-switcher " + stringify_flags();
    }

    void gen_render_instances(
        std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *shown_on)
        override;
};

class switcher_render_instance_t : public wf::scene::render_instance_t
{
    std::shared_ptr<switcher_node_t> self;
    wf::scene::damage_callback push_damage;

  public:
    switcher_render_instance_t(std::shared_ptr<switcher_node_t> node,
        wf::scene::damage_callback pd) : self(node), push_damage(pd)
    {}

    void schedule_instructions(
        std::vector<wf::scene::render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::regionf_t& damage) override
    {
        // Refresh each slot's buffer from the live window. need_render is set
        // when the slot's window changes (arrange), so between cycles the
        // buffers hold a static snapshot -- fine for a brief overview, and the
        // buffers are shared, so only the first output's pass renders them.
        for (auto& sl : self->slots)
        {
            if (!sl.need_render || !sl.view || !sl.view->is_mapped())
            {
                continue;
            }

            std::vector<wf::scene::render_instance_uptr> insts;
            sl.view->get_transformed_node()->gen_render_instances(
                insts, [] (auto) {});

            wf::render_target_t aux{sl.buffer};
            aux.geometry = sl.src;
            aux.scale    = sl.output ? sl.output->handle->scale : 1.0;

            wf::render_pass_params_t params;
            params.instances = &insts;
            params.damage    = sl.src;
            params.reference_output = sl.output;
            params.target = aux;
            wf::render_pass_t::run(params);

            sl.need_render = false;
        }

        instructions.push_back(wf::scene::render_instruction_t{
                .instance = this,
                .target   = target,
                .damage   = damage & self->get_bounding_box(),
            });
        damage ^= self->get_bounding_box();
    }

    void render(const wf::scene::render_instruction_t& data) override
    {
        data.pass->clear(data.damage, self->background);
        for (auto& sl : self->slots)
        {
            if (!sl.view)
            {
                continue;
            }

            auto tex = wf::texture_t::from_aux(sl.buffer);
            tex->set_filter_mode(WLR_SCALE_FILTER_BILINEAR);
            data.pass->add_texture(tex, data.target, sl.box, data.damage);

            // Wing dim: a translucent black wash. add_rect takes premultiplied
            // colour, so alpha a with zero RGB is {0, 0, 0, a}.
            if (sl.dim && (self->inactive_brightness < 1.0))
            {
                float a = (float)(1.0 - self->inactive_brightness);
                data.pass->add_rect({0, 0, 0, a}, data.target,
                    sl.box, data.damage);
            }
        }
    }
};

void switcher_node_t::gen_render_instances(
    std::vector<wf::scene::render_instance_uptr>& instances,
    wf::scene::damage_callback push_damage, wf::output_t *)
{
    // No shown_on filter: render on every output. The node is a scene-root
    // child, so each output clips the union to its own framebuffer.
    auto self = std::dynamic_pointer_cast<switcher_node_t>(shared_from_this());
    instances.push_back(
        std::make_unique<switcher_render_instance_t>(self, push_damage));
}
} // namespace

class global_switcher_t::impl
{
  public:
    bool all_ws;
    wf::color_t background;
    double inactive_brightness;
    std::vector<placed_view_t> views;   // MRU order, [0] = most recent
    int cursor = 0;
    std::shared_ptr<switcher_node_t> node;

    impl(bool all, wf::color_t bg, double ib) :
        all_ws(all), background(bg), inactive_brightness(ib)
    {}

    ~impl()
    {
        stop();
    }

    bool start()
    {
        views = all_ws ? all_ws_views() : current_ws_views();
        std::sort(views.begin(), views.end(),
            [] (const placed_view_t& a, const placed_view_t& b)
        {
            return wf::get_focus_timestamp(a.view) >
                   wf::get_focus_timestamp(b.view);
        });

        if (views.empty())
        {
            return false;
        }

        // Start on the previous window (classic alt+tab centres it right away).
        // NOTE: if the activating Tab press is also delivered to the grab, this
        // could land one past -- an off-by-one to confirm on the box.
        cursor = (views.size() > 1) ? 1 : 0;

        node = std::make_shared<switcher_node_t>(background,
            inactive_brightness);
        arrange();
        wf::scene::add_front(wf::get_core().scene(), node);
        return true;
    }

    void stop()
    {
        if (node)
        {
            wf::scene::remove_child(node);
            node = nullptr;
        }
    }

    void cycle(int dir)
    {
        if ((views.size() < 2) || !node)
        {
            return;
        }

        int n = (int)views.size();
        cursor = ((cursor + dir) % n + n) % n;
        arrange();
        for (auto *wo : wf::get_core().output_layout->get_outputs())
        {
            wo->render->damage_whole();
        }
    }

    wayfire_toplevel_view current_view() const
    {
        if (views.empty())
        {
            return nullptr;
        }

        return views[cursor].view;
    }

  private:
    // Rebuild the (up to) three visible slots from the cursor. Centre is the
    // cursor window; the wings are its ring neighbours, dimmed. Slot rectangles
    // are in global layout coordinates, so on three portrait panels the centre
    // lands on the middle monitor and the wings toward the side monitors.
    void arrange()
    {
        auto u = outputs_union();
        int n  = (int)views.size();
        int cx = u.x + u.width / 2;
        int cy = u.y + u.height / 2;

        std::vector<slot_t> s;
        auto make = [&] (int idx, int scx, int mw, int mh, bool dim)
        {
            auto& pv = views[idx];
            slot_t sl;
            sl.view   = pv.view;
            sl.output = pv.view->get_output();
            sl.src    = pv.view->get_transformed_node()->get_bounding_box();
            sl.box    = fit_centered(pv.box.width, pv.box.height, scx, cy,
                mw, mh);
            sl.dim    = dim;
            sl.buffer.allocate(wf::dimensions(sl.src),
                sl.output ? sl.output->handle->scale : 1.0,
                wf::buffer_allocation_hints_t{.needs_alpha = true});
            sl.need_render = true;
            s.push_back(std::move(sl));
        };

        make(cursor, cx, (int)(u.width * 0.30), (int)(u.height * 0.62), false);
        if (n > 1)
        {
            int prev = (cursor - 1 + n) % n;
            make(prev, u.x + (int)(u.width * 0.17),
                (int)(u.width * 0.20), (int)(u.height * 0.46), true);
        }

        if (n > 2)
        {
            int next = (cursor + 1) % n;
            make(next, u.x + (int)(u.width * 0.83),
                (int)(u.width * 0.20), (int)(u.height * 0.46), true);
        }

        node->slots = std::move(s);
    }
};

global_switcher_t::global_switcher_t(bool all_workspaces,
    wf::color_t background, double inactive_brightness) :
    priv(std::make_unique<impl>(all_workspaces, background,
        inactive_brightness))
{}

global_switcher_t::~global_switcher_t() = default;

bool global_switcher_t::start()
{
    return priv->start();
}

void global_switcher_t::cycle(int dir)
{
    priv->cycle(dir);
}

wayfire_toplevel_view global_switcher_t::current_view() const
{
    return priv->current_view();
}
} // namespace atlas
