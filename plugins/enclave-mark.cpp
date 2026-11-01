// enclave-mark: a compositor-drawn frame around every toplevel whose client
// connected through a security-context-v1 socket, coloured per sandbox
// instance, so "which Chrome is this?" has a visible answer.
//
// WHY A PLUGIN AND NOT A DECORATOR. The mark must not be suppressible by the
// client, and a server-side decorator only draws on windows that ACCEPT server
// decorations: Chrome, Slack and Electron default to drawing their own. So
// this is a separate scene node attached to the view, independent of who
// decorates it, drawn OUTSIDE the window geometry where no client pixel can
// cover or imitate it from within that window. A fullscreen window has no
// outside, and a fullscreen sandboxed client is exactly the one that could
// impersonate the whole desktop (a fake lock screen), so there the frame is
// drawn over the inner edge of the content instead.
//
// THE CONTEXT comes from wayfire/plugins/common/security-context.hpp, which
// the security-context-v1 plugin (wayfire-mods patch) fills in. With that
// plugin not loaded no client is sandboxed and nothing is marked.
//
// COLOUR: `colors` maps instance_id to a colour, as "r g b a" floats or an
// ESCAPED hex: `titan=1 0.5 0 1, work=\#3366FFFF`. An unescaped `#` starts a
// comment in wayfire.ini, so `titan=#FF8800FF` arrives as `titan=` (logged
// and skipped).
// LABEL: the instance name, on a tab in the frame colour, right-aligned on
// the frame's top edge and sitting above the window (inside the top-right
// corner when fullscreen), so it covers none of the client's pixels.
//
// An instance not listed gets a stable colour hashed from its name, so two
// enclaves always differ and an unlisted sandbox is still marked.

#include <wayfire/core.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/view.hpp>
#include <wayfire/plugins/common/cairo-util.hpp>
#include <wayfire/plugins/common/security-context.hpp>
#include <cmath>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace
{
const char *const MARK_DATA = "enclave-mark";

// FNV-1a: a stable hue per instance name, so an unlisted enclave's colour
// does not change between sessions.
wf::color_t hashed_color(const std::string& name)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : name)
    {
        h = (h ^ c) * 16777619u;
    }

    double hue = (h % 360) / 60.0;
    double x   = 1.0 - std::fabs(std::fmod(hue, 2.0) - 1.0);
    double r = 0, g = 0, b = 0;
    switch ((int)hue)
    {
      case 0: r = 1; g = x; break;
      case 1: r = x; g = 1; break;
      case 2: g = 1; b = x; break;
      case 3: g = x; b = 1; break;
      case 4: r = x; b = 1; break;
      default: r = 1; b = x; break;
    }

    return {r, g, b, 1.0};
}

std::string trim(const std::string& s)
{
    auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return "";
    }

    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}
}

// What every mark node reads: the plugin owns it and outlives the nodes.
struct mark_style_t
{
    int thickness = 4;
    bool label    = true;
    int label_size = 11;
    std::map<std::string, wf::color_t> colors;

    wf::color_t color_for(const std::string& instance) const
    {
        auto it = colors.find(instance);
        return it != colors.end() ? it->second : hashed_color(instance);
    }
};

class mark_node_t : public wf::scene::node_t
{
    std::weak_ptr<wf::toplevel_view_interface_t> view;
    const mark_style_t *style;
    std::string instance;
    wf::geometry_t last_box{0, 0, 0, 0};

    // The instance name, rendered at the scale it was last drawn at.
    wf::cairo_text_t label;
    float label_scale = 0;
    wf::color_t label_color{0, 0, 0, 0};
    int label_size = 0;
    wf::dimensions_t label_px{0, 0};

  public:
    mark_node_t(wayfire_toplevel_view view, const mark_style_t *style,
        std::string instance) : node_t(false), style(style),
        instance(std::move(instance))
    {
        this->view = std::dynamic_pointer_cast<wf::toplevel_view_interface_t>(
            view->shared_from_this());
    }

    // The window geometry (decorations included), in the coordinates of the
    // view's surface root, which is this node's parent.
    std::optional<wf::geometry_t> window_box()
    {
        auto v = view.lock();
        if (!v || !v->is_mapped())
        {
            return {};
        }

        auto g = v->get_geometry();
        auto o = v->get_surface_root_node()->to_global({0, 0});
        return wf::geometry_t{g.x - o.x, g.y - o.y, g.width, g.height};
    }

    bool fullscreen()
    {
        auto v = view.lock();
        return v && v->toplevel()->current().fullscreen;
    }

    // The frame's outer box: outside the window, or ON it when fullscreen.
    wf::geometry_t frame_box()
    {
        auto box = window_box();
        if (!box)
        {
            return {0, 0, 0, 0};
        }

        if (fullscreen())
        {
            return *box;
        }

        double t = style->thickness;
        return {box->x - t, box->y - t, box->width + 2 * t,
            box->height + 2 * t};
    }

    // Render the name if anything that shapes it changed: the scale it is
    // drawn at (crisp on HiDPI), the instance colour, or the font size.
    void update_label(float scale)
    {
        auto c = style->color_for(instance);
        if ((scale == label_scale) && (c == label_color) &&
            (style->label_size == label_size))
        {
            return;
        }

        // Black or white, whichever reads on the instance colour.
        double lum = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
        wf::color_t ink = lum > 0.5 ? wf::color_t{0, 0, 0, 1} :
            wf::color_t{1, 1, 1, 1};
        wf::cairo_text_t::params par(style->label_size, c, ink, scale);
        par.rounded_rect = false;
        // The texture must BE the tab: without exact_size the helper draws
        // the text centred on a larger default surface (measured 400x100 for
        // a 47x22 label), which scaled into the tab box became a dot.
        par.exact_size = true;
        label_px    = label.render_text(instance.empty() ? "sandbox" : instance,
            par);
        label_scale = scale;
        label_color = c;
        label_size  = style->label_size;
    }

    float output_scale()
    {
        auto v = view.lock();
        return (v && v->get_output()) ? v->get_output()->handle->scale : 1.0;
    }

    // The name tab: right-aligned on the frame's top edge, sitting ABOVE the
    // window so it covers none of the client's own pixels (for a client that
    // draws its own titlebar, that is where its tabs and buttons are). When
    // fullscreen there is no outside, so it tucks inside the top-right corner.
    std::optional<wf::geometry_t> tab_box()
    {
        auto box = window_box();
        if (!style->label || !box)
        {
            return {};
        }

        float scale = output_scale();
        update_label(scale);
        double w = label_px.width / scale, h = label_px.height / scale;
        double t = style->thickness;
        if (w + 2 * t > box->width)
        {
            return {};   // no room: the frame alone still marks it
        }

        if (fullscreen())
        {
            return wf::geometry_t{box->x + box->width - t - w, box->y + t,
                w, h};
        }

        return wf::geometry_t{box->x + box->width + t - w, box->y - t - h,
            w, h};
    }

    wf::geometry_t get_bounding_box() override
    {
        auto f   = frame_box();
        auto tab = tab_box();
        if (!tab)
        {
            return f;
        }

        double x1 = std::min(f.x, tab->x), y1 = std::min(f.y, tab->y);
        double x2 = std::max(f.x + f.width, tab->x + tab->width);
        double y2 = std::max(f.y + f.height, tab->y + tab->height);
        return {x1, y1, x2 - x1, y2 - y1};
    }

    // Damage where the frame was and where it is now (after a move, resize
    // or fullscreen change), so neither a stale nor a missing edge remains.
    void refresh()
    {
        auto self = shared_from_this();
        wf::scene::damage_node(self, last_box);
        last_box = get_bounding_box();
        wf::scene::damage_node(self, last_box);
    }

    void render(const wf::scene::render_instruction_t& data)
    {
        render_frame(data);
        if (auto tab = tab_box())
        {
            update_label(data.target.scale);
            data.pass->add_texture(label.get_texture(), data.target, *tab,
                data.damage);
        }
    }

    void render_frame(const wf::scene::render_instruction_t& data)
    {
        auto b = frame_box();
        double t = style->thickness;
        if ((b.width <= 2 * t) || (b.height <= 2 * t))
        {
            return;
        }

        auto c = style->color_for(instance);
        auto& p = *data.pass;
        p.add_rect(c, data.target, {b.x, b.y, b.width, t}, data.damage);
        p.add_rect(c, data.target, {b.x, b.y + b.height - t, b.width, t},
            data.damage);
        p.add_rect(c, data.target, {b.x, b.y + t, t, b.height - 2 * t},
            data.damage);
        p.add_rect(c, data.target,
            {b.x + b.width - t, b.y + t, t, b.height - 2 * t}, data.damage);
    }

    class render_instance_t : public wf::scene::render_instance_t
    {
        std::shared_ptr<mark_node_t> self;
        wf::scene::damage_callback push_damage;
        wf::signal::connection_t<wf::scene::node_damage_signal> on_damage =
            [=] (wf::scene::node_damage_signal *ev)
        {
            push_damage(ev->region);
        };

      public:
        render_instance_t(mark_node_t *self, wf::scene::damage_callback push)
        {
            this->self = std::dynamic_pointer_cast<mark_node_t>(
                self->shared_from_this());
            this->push_damage = push;
            self->connect(&on_damage);
        }

        void schedule_instructions(
            std::vector<wf::scene::render_instruction_t>& instructions,
            const wf::render_target_t& target, wf::regionf_t& damage) override
        {
            wf::regionf_t ours = damage & self->get_bounding_box();
            if (!ours.empty())
            {
                instructions.push_back(wf::scene::render_instruction_t{
                    .instance = this,
                    .target   = target,
                    .damage   = std::move(ours),
                });
            }
        }

        void render(const wf::scene::render_instruction_t& data) override
        {
            self->render(data);
        }
    };

    void gen_render_instances(
        std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *output) override
    {
        instances.push_back(std::make_unique<render_instance_t>(this,
            push_damage));
    }
};

// Stored on the view, so a view is marked at most once and fini can find it.
struct mark_data_t : public wf::custom_data_t
{
    std::shared_ptr<mark_node_t> node;
};

class wayfire_enclave_mark : public wf::plugin_interface_t
{
    wf::option_wrapper_t<int> thickness{"enclave-mark/thickness"};
    wf::option_wrapper_t<bool> label{"enclave-mark/label"};
    wf::option_wrapper_t<int> label_size{"enclave-mark/label_size"};
    wf::option_wrapper_t<std::string> colors{"enclave-mark/colors"};
    mark_style_t style;

    void load_style()
    {
        style.thickness  = std::max(1, (int)thickness);
        style.label      = label;
        style.label_size = std::max(6, (int)label_size);
        style.colors.clear();
        std::istringstream iss((std::string)colors);
        std::string entry;
        while (std::getline(iss, entry, ','))
        {
            auto eq = entry.find('=');
            auto c  = wf::option_type::from_string<wf::color_t>(
                trim(entry.substr(eq == std::string::npos ? 0 : eq + 1)));
            if ((eq == std::string::npos) || !c)
            {
                LOGE("enclave-mark: ignoring bad colors entry '", entry, "'");
                continue;
            }

            style.colors[trim(entry.substr(0, eq))] = *c;
        }

        for (auto& view : wf::get_core().get_all_views())
        {
            if (auto d = view->get_data<mark_data_t>(MARK_DATA))
            {
                d->node->refresh();
            }
        }
    }

    void attach(wayfire_view view)
    {
        auto toplevel = wf::toplevel_cast(view);
        if (!toplevel || toplevel->has_data(MARK_DATA))
        {
            return;
        }

        auto ctx = wf::security_context::lookup_client(view->get_client());
        if (!ctx)
        {
            return;
        }

        auto data  = std::make_unique<mark_data_t>();
        data->node = std::make_shared<mark_node_t>(toplevel, &style,
            ctx->instance_id);
        wf::scene::add_front(toplevel->get_surface_root_node(), data->node);
        data->node->refresh();
        LOGI("enclave-mark: marked view ", view->get_id(), " (instance '",
            ctx->instance_id, "', engine '", ctx->sandbox_engine, "')");
        toplevel->store_data(std::move(data), MARK_DATA);
    }

    void refresh(wayfire_view view)
    {
        if (auto d = view ? view->get_data<mark_data_t>(MARK_DATA) : nullptr)
        {
            d->node->refresh();
        }
    }

    wf::signal::connection_t<wf::view_mapped_signal> on_mapped =
        [=] (wf::view_mapped_signal *ev) { attach(ev->view); };
    wf::signal::connection_t<wf::view_geometry_changed_signal> on_geometry =
        [=] (wf::view_geometry_changed_signal *ev) { refresh(ev->view); };
    wf::signal::connection_t<wf::view_fullscreen_signal> on_fullscreen =
        [=] (wf::view_fullscreen_signal *ev) { refresh(ev->view); };

  public:
    void init() override
    {
        load_style();
        thickness.set_callback([=] { load_style(); });
        label.set_callback([=] { load_style(); });
        label_size.set_callback([=] { load_style(); });
        colors.set_callback([=] { load_style(); });
        wf::get_core().connect(&on_mapped);
        wf::get_core().connect(&on_geometry);
        wf::get_core().connect(&on_fullscreen);
        for (auto& view : wf::get_core().get_all_views())
        {
            if (view->is_mapped())
            {
                attach(view);
            }
        }
    }

    void fini() override
    {
        for (auto& view : wf::get_core().get_all_views())
        {
            if (auto d = view->get_data<mark_data_t>(MARK_DATA))
            {
                auto node = d->node;
                wf::scene::damage_node(node, node->get_bounding_box());
                wf::scene::remove_child(node);
                view->erase_data(MARK_DATA);
            }
        }
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_enclave_mark);
