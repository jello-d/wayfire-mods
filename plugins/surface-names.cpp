// surface-names.cpp
//
// Name the parts of each output on a workspace change, and on a toggle.
//
// A REPLACEMENT FOR wayfire-plugins-extra's workspace-names, which draws ONE
// label per output per workspace, centred. This draws ANY NUMBER per output
// per workspace, each centred (or placed by `position`) in its OWN rectangle,
// given as fractions of the output: a portrait panel split top and bottom
// shows two labels, one in the middle of each half. Everything else is the
// same plugin: the same overlay layer, the same rounded-box look, the same
// fade, the same display_duration on a workspace change (edge flipping, expo,
// vswitch, a raw IPC request: anything that changes the workspace).
//
// PLUS A TOGGLE (`toggle`, an activator binding, also an IPC method of the
// same name): pinned on, every output shows its labels and keeps them,
// following workspace changes, until toggled off. One toggle for the whole
// desk, so all outputs show and hide together.
//
// THE INPUT IS GENERIC. `labels` is a dynamic list whose entries are keyed
// `<output>_workspace_<N>_<k>` (N row-major from 1, as workspace-names
// counts; k any number, distinct per workspace) with the value
// `<fx> <fy> <fw> <fh> <text>`, the rect as fractions of the output. This
// plugin knows nothing about where they come from; the integrator writes
// them. A workspace with no entry for an output shows "Workspace N" over the
// whole output, as workspace-names did, unless `show_unlabelled` is off.
//
// Based on workspace-names.cpp (MIT, Copyright (c) 2026 Scott Moreau): the
// scene node, the render instance and the cairo label are its, adapted.

#include <wayfire/bindings.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/workarea.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/region.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/signal-provider.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/util.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/plugins/common/cairo-util.hpp>
#include <wayfire/plugins/ipc/ipc-activator.hpp>
#include <wayfire/config/compound-option.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#define WIDGET_PADDING 20

namespace surface_names
{
// One label: where it sits inside its workspace (output-local), its text and
// its texture.
struct label_t
{
    wf::geometry_t rect{0, 0, 0, 0};
    std::string text;
    std::unique_ptr<wf::owned_texture_t> texture;
};

// Pinned by the toggle: plugin-wide, since one key shows every output.
static bool g_pinned = false;

class label_render_instance_t : public wf::scene::render_instance_t
{
    wf::signal::connection_t<wf::scene::node_damage_signal> on_damaged =
        [=] (wf::scene::node_damage_signal *ev) { push_to_parent(ev->region); };

    wf::scene::node_t *self;
    wf::scene::damage_callback push_to_parent;
    std::shared_ptr<label_t> label;
    wf::point_t *offset;
    double *alpha;

  public:
    label_render_instance_t(wf::scene::node_t *self,
        wf::scene::damage_callback push, wf::point_t *offset, double *alpha,
        std::shared_ptr<label_t> label) :
        self(self), push_to_parent(push), label(label), offset(offset),
        alpha(alpha)
    {
        self->connect(&on_damaged);
    }

    void schedule_instructions(
        std::vector<wf::scene::render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::regionf_t& damage) override
    {
        instructions.push_back(wf::scene::render_instruction_t{
                .instance = this,
                .target   = target,
                .damage   = damage & self->get_bounding_box(),
            });
    }

    void render(const wf::scene::render_instruction_t& data) override
    {
        if (!label->texture)
        {
            return;
        }

        wf::geometry_t g{label->rect.x + offset->x, label->rect.y + offset->y,
            label->rect.width, label->rect.height};
        data.pass->custom_gles_subpass(data.target, [&]
        {
            wf::gles::for_each_scissor_rect(data.target, data.damage, [&]
            {
                OpenGL::render_texture(
                    wf::gles_texture_t{label->texture->get_texture()},
                    data.target, g, glm::vec4(1, 1, 1, *alpha), 0);
            });
            wf::scene::damage_node(self, g);
        });
    }
};

class label_node_t : public wf::scene::node_t
{
    wf::point_t offset{0, 0};
    double alpha = 0.0;

  public:
    std::shared_ptr<label_t> label = std::make_shared<label_t>();
    wf::point_t ws;

    explicit label_node_t(wf::point_t ws) : node_t(false), ws(ws)
    {}

    void gen_render_instances(
        std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t*) override
    {
        instances.push_back(std::make_unique<label_render_instance_t>(
            this, push_damage, &offset, &alpha, label));
    }

    wf::geometry_t get_bounding_box() override
    {
        return {label->rect.x + offset.x, label->rect.y + offset.y,
            label->rect.width, label->rect.height};
    }

    void set_offset(wf::point_t o)
    {
        offset = o;
    }

    void set_alpha(double a)
    {
        alpha = a;
    }
};

class surface_names_output_t : public wf::per_output_plugin_instance_t
{
    wf::wl_timer<false> timer;
    bool hook_set = false;
    std::vector<std::shared_ptr<label_node_t>> nodes;

    wf::option_wrapper_t<std::string> font{"surface-names/font"};
    wf::option_wrapper_t<std::string> position{"surface-names/position"};
    wf::option_wrapper_t<int> display_duration{
        "surface-names/display_duration"};
    wf::option_wrapper_t<int> margin{"surface-names/margin"};
    wf::option_wrapper_t<double> background_radius{
        "surface-names/background_radius"};
    wf::option_wrapper_t<wf::color_t> text_color{"surface-names/text_color"};
    wf::option_wrapper_t<wf::color_t> background_color{
        "surface-names/background_color"};
    wf::option_wrapper_t<bool> show_unlabelled{
        "surface-names/show_unlabelled"};
    wf::option_wrapper_t<wf::config::compound_list_t<std::string>> labels{
        "surface-names/labels"};
    wf::animation::simple_animation_t alpha_fade{display_duration};

  public:
    void init() override
    {
        alpha_fade.set(0, 0);
        output->wset()->connect(&grid_changed);
        output->connect(&workarea_changed);
        output->connect(&viewport_changed);
        output->connect(&viewport_change_request);
        font.set_callback(option_changed);
        position.set_callback(option_changed);
        margin.set_callback(option_changed);
        background_radius.set_callback(option_changed);
        background_color.set_callback(option_changed);
        text_color.set_callback(option_changed);
        show_unlabelled.set_callback(option_changed);
        labels.set_callback(option_changed);
        if (g_pinned)
        {
            show();
        }
    }

    void fini() override
    {
        deactivate();
        grid_changed.disconnect();
        viewport_change_request.disconnect();
        viewport_changed.disconnect();
        workarea_changed.disconnect();
        output->render->damage_whole();
    }

    // SHOW: on a workspace change, for display_duration then fade; pinned,
    // until hidden. Either way the labels follow the current workspace.
    void show()
    {
        activate();
        place();
        alpha_fade.animate(alpha_fade, 1);
        timer.disconnect();
        if (!g_pinned)
        {
            timer.set_timeout((int)display_duration, timeout);
        }

        output->render->damage_whole();
    }

    void hide()
    {
        timer.disconnect();
        alpha_fade.animate(alpha_fade, 0);
        output->render->damage_whole();
    }

  private:
    // The entries for THIS output: (workspace number, fraction rect, text).
    struct entry_t
    {
        int ws;
        double fx, fy, fw, fh;
        std::string text;
    };

    std::vector<entry_t> entries()
    {
        std::vector<entry_t> out;
        std::string prefix = output->to_string() + "_workspace_";
        for (const auto& [key, value] : labels.value())
        {
            if (key.rfind(prefix, 0) != 0)
            {
                continue;
            }

            int ws, k;
            if (sscanf(key.c_str() + prefix.size(), "%d_%d", &ws, &k) != 2)
            {
                continue;
            }

            entry_t e{ws, 0, 0, 1, 1, ""};
            int used = 0;
            if (sscanf(value.c_str(), "%lf %lf %lf %lf %n", &e.fx, &e.fy,
                &e.fw, &e.fh, &used) < 4)
            {
                LOGW("surface-names: ", key, ": expected `fx fy fw fh text`");
                continue;
            }

            e.text = value.substr(used);
            if (!e.text.empty())
            {
                out.push_back(e);
            }
        }

        return out;
    }

    void build_nodes()
    {
        auto grid = output->wset()->get_workspace_grid_size();
        auto all  = entries();
        for (int y = 0; y < grid.height; y++)
        {
            for (int x = 0; x < grid.width; x++)
            {
                int n = y * grid.width + x + 1;
                bool any = false;
                for (auto& e : all)
                {
                    if (e.ws == n)
                    {
                        add_node({x, y}, e);
                        any = true;
                    }
                }

                if (!any && show_unlabelled)
                {
                    add_node({x, y}, {n, 0, 0, 1, 1,
                        "Workspace " + std::to_string(n)});
                }
            }
        }
    }

    void add_node(wf::point_t ws, const entry_t& e)
    {
        auto node = std::make_shared<label_node_t>(ws);
        node->label->text = e.text;
        render_label(node->label, region_of(e));
        wf::scene::add_front(
            output->node_for_layer(wf::scene::layer::OVERLAY), node);
        nodes.push_back(node);
    }

    // The entry's rect in output-local pixels, kept inside the workarea so a
    // label never sits under a bar.
    wf::geometry_t region_of(const entry_t& e)
    {
        auto og = output->get_relative_geometry();
        // wf::geometry_t holds doubles here; whole pixels are what we mean.
        wf::geometry_t r{std::round(e.fx * og.width),
            std::round(e.fy * og.height), std::round(e.fw * og.width),
            std::round(e.fh * og.height)};
        auto wa = output->workarea->get_workarea();
        double x1 = std::max(r.x, wa.x), y1 = std::max(r.y, wa.y);
        double x2 = std::min(r.x + r.width, wa.x + wa.width);
        double y2 = std::min(r.y + r.height, wa.y + wa.height);
        if ((x2 > x1) && (y2 > y1))
        {
            return {x1, y1, x2 - x1, y2 - y1};
        }

        return r;
    }

    // Render the label's box and text, sized as workspace-names sizes it
    // (5% of the output's height), SHRUNK to fit its region's width, and
    // placed in the region by `position`.
    void render_label(std::shared_ptr<label_t> l, wf::geometry_t region)
    {
        auto og = output->get_relative_geometry();
        double size = og.height * 0.05;
        cairo_text_extents_t ext;
        auto measure = [&] (double s)
        {
            auto *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
            auto *cr   = cairo_create(surf);
            cairo_select_font_face(cr, std::string(font).c_str(),
                CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, s);
            cairo_text_extents(cr, l->text.c_str(), &ext);
            cairo_destroy(cr);
            cairo_surface_destroy(surf);
        };
        measure(size);
        double room = region.width - 2.0 * WIDGET_PADDING - 2.0 * margin;
        if ((ext.width > room) && (room > 0))
        {
            size *= room / ext.width;
            measure(size);
        }

        int w = std::max(1, int(ext.width + WIDGET_PADDING * 2));
        int h = std::max(1, int(ext.height + WIDGET_PADDING * 2));
        auto *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        auto *cr   = cairo_create(surf);
        double r   = std::min<double>(background_radius, std::min(w, h) / 2.0);
        wf::color_t bg = background_color, fg = text_color;
        cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
        cairo_new_path(cr);
        cairo_arc(cr, r, h - r, r, M_PI / 2, M_PI);
        cairo_line_to(cr, 0, r);
        cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
        cairo_line_to(cr, w - r, 0);
        cairo_arc(cr, w - r, r, r, 3 * M_PI / 2, 2 * M_PI);
        cairo_line_to(cr, w, h - r);
        cairo_arc(cr, w - r, h - r, r, 0, M_PI / 2);
        cairo_close_path(cr);
        cairo_fill(cr);
        cairo_select_font_face(cr, std::string(font).c_str(),
            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, size);
        cairo_set_source_rgba(cr, fg.r, fg.g, fg.b, fg.a);
        cairo_move_to(cr, w / 2.0 - (ext.width / 2 + ext.x_bearing),
            h / 2.0 - (ext.height / 2 + ext.y_bearing));
        cairo_show_text(cr, l->text.c_str());
        cairo_destroy(cr);
        l->texture = std::make_unique<wf::owned_texture_t>(surf);
        cairo_surface_destroy(surf);
        l->rect = place_in(region, w, h);
    }

    wf::geometry_t place_in(wf::geometry_t r, int w, int h)
    {
        std::string p = position;
        int m = margin;
        int x = int(r.x + (r.width - w) / 2), y = int(r.y + (r.height - h) / 2);
        if (p.find("left") != std::string::npos)
        {
            x = int(r.x) + m;
        }

        if (p.find("right") != std::string::npos)
        {
            x = int(r.x + r.width) - w - m;
        }

        if (p.rfind("top", 0) == 0)
        {
            y = int(r.y) + m;
        }

        if (p.rfind("bottom", 0) == 0)
        {
            y = int(r.y + r.height) - h - m;
        }

        return {double(x), double(y), double(w), double(h)};
    }

    void place()
    {
        auto cur = output->wset()->get_current_workspace();
        auto og  = output->get_relative_geometry();
        for (auto& n : nodes)
        {
            n->set_offset({int((n->ws.x - cur.x) * og.width),
                int((n->ws.y - cur.y) * og.height)});
        }
    }

    void set_alpha()
    {
        for (auto& n : nodes)
        {
            n->set_alpha(alpha_fade);
        }
    }

    void clear_nodes()
    {
        for (auto& n : nodes)
        {
            wf::scene::remove_child(n);
        }

        nodes.clear();
    }

    void activate()
    {
        if (hook_set)
        {
            return;
        }

        build_nodes();
        output->render->add_effect(&post_hook, wf::OUTPUT_EFFECT_POST);
        output->render->add_effect(&pre_hook, wf::OUTPUT_EFFECT_PRE);
        hook_set = true;
    }

    void deactivate()
    {
        if (!hook_set)
        {
            return;
        }

        timer.disconnect();
        clear_nodes();
        output->render->rem_effect(&post_hook);
        output->render->rem_effect(&pre_hook);
        hook_set = false;
    }

    // Anything that changes what or where a label is: rebuild, if showing.
    void rebuild()
    {
        if (!hook_set)
        {
            return;
        }

        clear_nodes();
        build_nodes();
        place();
        set_alpha();
        output->render->damage_whole();
    }

    wf::config::option_base_t::updated_callback_t option_changed =
        [=] () { rebuild(); };

    wf::signal::connection_t<wf::workspace_grid_changed_signal> grid_changed =
        [=] (wf::workspace_grid_changed_signal*) { rebuild(); };

    wf::signal::connection_t<wf::workarea_changed_signal> workarea_changed =
        [=] (wf::workarea_changed_signal*) { rebuild(); };

    wf::signal::connection_t<wf::workspace_change_request_signal>
    viewport_change_request =
        [=] (wf::workspace_change_request_signal*) { show(); };

    wf::signal::connection_t<wf::workspace_changed_signal> viewport_changed =
        [=] (wf::workspace_changed_signal*) { show(); };

    wf::wl_timer<false>::callback_t timeout = [=] ()
    {
        output->render->damage_whole();
        alpha_fade.animate(alpha_fade, 0);
    };

    wf::effect_hook_t pre_hook = [=] ()
    {
        if (alpha_fade.running())
        {
            set_alpha();
        }
    };

    wf::effect_hook_t post_hook = [=] ()
    {
        if (!alpha_fade.running() && !timer.is_connected() && !g_pinned)
        {
            deactivate();
        } else
        {
            set_alpha();
        }
    };
};

class surface_names_t : public wf::plugin_interface_t,
    public wf::per_output_tracker_mixin_t<surface_names_output_t>
{
    wf::ipc_activator_t toggle{"surface-names/toggle"};

  public:
    void init() override
    {
        init_output_tracking();
        toggle.set_handler([=] (wf::output_t*, wayfire_view)
        {
            g_pinned = !g_pinned;
            for (auto& [wo, inst] : output_instance)
            {
                if (g_pinned)
                {
                    inst->show();
                } else
                {
                    inst->hide();
                }
            }

            return true;
        });
        LOGI("SURFACE-NAMES init: BUILD=", __DATE__, " ", __TIME__);
    }

    void fini() override
    {
        g_pinned = false;
        fini_output_tracking();
    }
};
}

DECLARE_WAYFIRE_PLUGIN(surface_names::surface_names_t);
