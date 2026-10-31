// global-ring.cpp: see global-ring.hpp. The spanning aux-buffer + add_texture
// overlay is the same path as global-switcher.cpp (and global-wall.cpp before
// it): a scene-root node that clears to a backdrop and textures each window's
// live content into its slot, so every output clips the union to its own
// framebuffer and nothing but the arranged thumbnails is drawn. The only
// difference from the switcher is the layout: a near-square grid of ALL the
// collected windows, not a 3-slot MRU ring.

#include "global-ring.hpp"
#include "atlas-anim.hpp"
#include "../global-wset.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/region.hpp>
#include <wayfire/render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/plugins/common/util.hpp>

// Image loading for the ring's wallpaper backdrop. This TU is the single place
// in atlas.so that pulls in the stb implementation; STATIC keeps its symbols
// internal so they cannot clash with the copy in workspace-wallpaper.so.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include <stb/stb_image.h>

namespace atlas
{
namespace
{
// Bounding box of every output in layout coordinates: the logical desktop.
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

// Split `area` into the smallest near-square grid holding n cells, `gap` px
// around and between them. One rect per index, row-major.
std::vector<wf::geometry_t> grid_cells(size_t n, wf::geometry_t area, int gap)
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

// Assign each view to a distinct grid cell so the TOTAL travel from every
// window's real on-screen position to its cell is minimised: the linear
// assignment problem, solved exactly by the Hungarian (Kuhn-Munkres) method.
// Without it, cells are handed out in enumeration order and a window can sweep
// clear across the desktop to a far cell; with it, each window drifts the short
// way to a free slot and the grid keeps the desktop's rough left-right /
// top-bottom layout. Runs once at ring open, n is the window count, so the
// O(n^3) cost is irrelevant.
//
// cost[i][j] is the squared pixel distance between view i's centre and cell j's
// centre (squared skips a sqrt and leans a little harder against long moves,
// which is the intent). n views, n cells -> a square assignment. Returns, per
// view index, the cell index it should occupy.
std::vector<int> min_movement_assignment(
    const std::vector<wf::point_t>& view_c,
    const std::vector<wf::point_t>& cell_c)
{
    const int n = (int)view_c.size();
    std::vector<int> assign(std::max(0, n), 0);
    if (n == 0)
    {
        return assign;
    }

    const long long INF = std::numeric_limits<long long>::max() / 4;
    auto cost = [&](int i, int j) -> long long   // 1-indexed
    {
        long long dx = (long long)view_c[i - 1].x - cell_c[j - 1].x;
        long long dy = (long long)view_c[i - 1].y - cell_c[j - 1].y;
        return dx * dx + dy * dy;
    };

    // Standard O(n^3) Kuhn-Munkres with potentials (u, v). p[j] tracks the view
    // assigned to cell j; way[] records the alternating path for augmentation.
    std::vector<long long> u(n + 1, 0), v(n + 1, 0);
    std::vector<int> p(n + 1, 0), way(n + 1, 0);
    for (int i = 1; i <= n; i++)
    {
        p[0] = i;
        int j0 = 0;
        std::vector<long long> minv(n + 1, INF);
        std::vector<bool> used(n + 1, false);
        do {
            used[j0] = true;
            int i0 = p[j0], j1 = -1;
            long long delta = INF;
            for (int j = 1; j <= n; j++)
            {
                if (used[j])
                {
                    continue;
                }

                long long cur = cost(i0, j) - u[i0] - v[j];
                if (cur < minv[j])
                {
                    minv[j] = cur;
                    way[j]  = j0;
                }

                if (minv[j] < delta)
                {
                    delta = minv[j];
                    j1    = j;
                }
            }

            for (int j = 0; j <= n; j++)
            {
                if (used[j])
                {
                    u[p[j]] += delta;
                    v[j]    -= delta;
                } else
                {
                    minv[j] -= delta;
                }
            }

            j0 = j1;
        } while (p[j0] != 0);

        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }

    for (int j = 1; j <= n; j++)   // p[j] = view (1-indexed) at cell j
    {
        assign[p[j] - 1] = j - 1;
    }

    return assign;
}

// Interpolate rect a -> b at t in [0, 1] (t already smoothed by the animation).
wf::geometry_t lerp_geometry(wf::geometry_t a, wf::geometry_t b, double t)
{
    return {
        (int)std::lround(a.x + (b.x - a.x) * t),
        (int)std::lround(a.y + (b.y - a.y) * t),
        (int)std::lround(a.width + (b.width - a.width) * t),
        (int)std::lround(a.height + (b.height - a.height) * t)};
}

// Fit a vw x vh box into `cell`, preserving aspect, centred, never upscaling.
wf::geometry_t fit_in(int vw, int vh, wf::geometry_t cell)
{
    int cx = cell.x + cell.width / 2;
    int cy = cell.y + cell.height / 2;
    if ((vw <= 0) || (vh <= 0))
    {
        return {cx, cy, 0, 0};
    }

    double s = std::min({(double)cell.width / vw, (double)cell.height / vh,
        1.0});
    int w = (int)std::lround(vw * s);
    int h = (int)std::lround(vh * s);
    return {cx - w / 2, cy - h / 2, w, h};
}

std::string expand_home(const std::string& in)
{
    if (!in.empty() && (in[0] == '~') && (in.size() == 1 || in[1] == '/'))
    {
        const char *home = getenv("HOME");
        if (home)
        {
            return std::string(home) + in.substr(1);
        }
    }

    return in;
}

// The wallpaper filename mapped to workspace cell `cell` in the manifest (the
// same "x y filename" map the workspace-wallpaper plugin reads). "" if the
// manifest is unreadable or has no entry for that cell.
std::string manifest_lookup(const std::string& path, wf::point_t cell)
{
    std::ifstream in(path);
    if (!in)
    {
        return "";
    }

    std::string line;
    while (std::getline(in, line))
    {
        size_t a = line.find_first_not_of(" \t");
        if ((a == std::string::npos) || (line[a] == '#'))
        {
            continue;
        }

        std::istringstream ss(line.substr(a));
        int x, y;
        if (!(ss >> x >> y) || (x != cell.x) || (y != cell.y))
        {
            continue;
        }

        std::string file;
        std::getline(ss, file);
        size_t b = file.find_first_not_of(" \t");
        size_t c = file.find_last_not_of(" \t\r\n");
        if (b == std::string::npos)
        {
            return "";
        }

        return file.substr(b, c - b + 1);
    }

    return "";
}

// Cover-fit an img_w x img_h image over `span` (FILL: scale to cover, centred,
// may overflow): identical to the workspace-wallpaper plugin's FILL mode, so
// the ring backdrop matches the live desktop wallpaper exactly.
wf::geometry_t fit_cover(int iw, int ih, wf::geometry_t span)
{
    if ((iw <= 0) || (ih <= 0))
    {
        return span;
    }

    double s  = std::max((double)span.width / iw, (double)span.height / ih);
    int dw = (int)std::lround(iw * s);
    int dh = (int)std::lround(ih * s);
    return {span.x + (span.width - dw) / 2, span.y + (span.height - dh) / 2,
        dw, dh};
}

// The current workspace's wallpaper, loaded into a GL texture and cover-fitted
// over `span` (the output union, global coords). Drawn directly by the ring's
// render() with OpenGL::render_texture: NOT captured from the plugin's
// BACKGROUND node into an aux buffer, which only rendered correctly for the
// origin-0 output. std::nullopt if there is no wallpaper for the current cell.
struct wallpaper_t
{
    GLuint tex = 0;
    wf::geometry_t cover{0, 0, 0, 0};
};

std::optional<wallpaper_t> load_current_wallpaper(wf::geometry_t span)
{
    wf::point_t cur{0, 0};
    for (auto *wo : wf::get_core().output_layout->get_outputs())
    {
        if (wo->wset())
        {
            cur = wo->wset()->get_current_workspace();
            break;
        }
    }

    wf::option_wrapper_t<std::string> dir_opt{"workspace-wallpaper/directory"};
    wf::option_wrapper_t<std::string> mf_opt{"workspace-wallpaper/manifest"};
    std::string dir      = expand_home((std::string)dir_opt);
    std::string manifest = expand_home((std::string)mf_opt);

    std::string file = manifest.empty() ? "" : manifest_lookup(manifest, cur);
    if (file.empty())
    {
        return {};
    }

    std::string path;
    if ((file[0] == '/') || (file[0] == '~'))
    {
        path = expand_home(file);
    } else
    {
        path = dir + "/" + file;
    }

    int iw = 0, ih = 0, comp = 0;
    stbi_uc *data = stbi_load(path.c_str(), &iw, &ih, &comp, 4);
    if (!data)
    {
        LOGW("atlas ring: wallpaper load failed: ", path, " (",
            stbi_failure_reason(), ")");
        return {};
    }

    GLuint tex = 0;
    wf::gles::run_in_context([&]
    {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_RGBA,
            GL_UNSIGNED_BYTE, data);
    });
    stbi_image_free(data);

    if (!tex)
    {
        return {};
    }

    return wallpaper_t{tex, fit_cover(iw, ih, span)};
}

// One grid slot: a window rendered into its own buffer and textured into `box`
// (global layout coordinates). `src` is the window's content bbox in its own
// output-local space: the buffer size and the render source.
struct slot_t
{
    wayfire_toplevel_view view;
    wf::output_t *output;
    wf::geometry_t src;    // window content bbox, output-local (render source)
    wf::geometry_t start;  // real on-screen rect, global (animation start)
    wf::geometry_t box;    // grid cell, global (animation end)
    wf::auxilliary_buffer_t buffer;
    bool need_render = true;
};

class ring_node_t : public wf::scene::node_t
{
  public:
    wf::color_t background;
    wf::geometry_t u{0, 0, 0, 0};
    std::vector<slot_t> slots;

    // The current workspace's wallpaper, drawn behind the thumbnails so the
    // overview reads as a reshuffle on the real desktop, not a switch to black.
    // Loaded once at open; cover is in global (union) coords.
    GLuint wp_tex = 0;
    wf::geometry_t wp_cover{0, 0, 0, 0};

    // Open/close animation progress, owned by impl's shared driver: 0 at the
    // real window positions, 1 at the grid layout. render() interpolates each
    // thumbnail between the two by t().
    const anim_driver_t *driver = nullptr;

    ring_node_t(wf::color_t bg) : node_t(false), background(bg)
    {
        u = outputs_union();
    }

    ~ring_node_t() override
    {
        if (wp_tex)
        {
            GLuint t = wp_tex;
            wf::gles::run_in_context([&] { glDeleteTextures(1, &t); });
            wp_tex = 0;
        }
    }

    double t() const
    {
        return driver ? driver->progress() : 1.0;
    }

    // Spans the whole logical desktop, so the overlay suppresses the normal
    // desktop on every output (each output clips the union to its framebuffer).
    wf::geometry_t get_bounding_box() override
    {
        return u;
    }

    std::string stringify() const override
    {
        return "atlas-global-ring " + stringify_flags();
    }

    void gen_render_instances(
        std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *shown_on)
        override;
};

class ring_render_instance_t : public wf::scene::render_instance_t
{
    std::shared_ptr<ring_node_t> self;
    wf::scene::damage_callback push_damage;

  public:
    ring_render_instance_t(std::shared_ptr<ring_node_t> node,
        wf::scene::damage_callback pd) : self(node), push_damage(pd)
    {}

    void schedule_instructions(
        std::vector<wf::scene::render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::regionf_t& damage) override
    {
        // Refresh each slot's buffer from the live window (once per slot: the
        // buffers are shared across the per-output instances, so only the first
        // output's pass renders them).
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
        // Dark floor first (covers any letterbox the cover-fit leaves), then
        // the real wallpaper, then the window thumbnails on top.
        data.pass->clear(data.damage, self->background);

        // The current workspace's wallpaper, cover-fitted over the union and
        // drawn per output: data.damage scissors each output to its own
        // slice, the same global-coordinate model the thumbnails use, so it
        // lands correctly on every output (unlike the old aux-capture path,
        // which only rendered the origin-0 output). Drawn straight to the ring
        // target with OpenGL::render_texture, no intermediate buffer.
        if (self->wp_tex)
        {
            const GLuint tex   = self->wp_tex;
            const wf::geometry_t cover = self->wp_cover;
            data.pass->custom_gles_subpass([&]
            {
                wf::gles::bind_render_buffer(data.target);
                // 0.11: damage boxes are pixman_box64f_t (float) and
                // render_target_logic_scissor takes one directly: no
                // wlr_box_from_pixman_box int conversion any more.
                for (const auto& box : data.damage)
                {
                    wf::gles::render_target_logic_scissor(data.target, box);
                    OpenGL::render_texture(wf::gles_texture_t{tex},
                        data.target, cover, glm::vec4(1.0),
                        OpenGL::TEXTURE_TRANSFORM_INVERT_Y);
                }
            });
        }

        const double t = self->t();
        for (auto& sl : self->slots)
        {
            if (!sl.view)
            {
                continue;
            }

            auto tex = wf::texture_t::from_aux(sl.buffer);
            tex->set_filter_mode(WLR_SCALE_FILTER_BILINEAR);
            const auto cur = lerp_geometry(sl.start, sl.box, t);
            data.pass->add_texture(tex, data.target, cur, data.damage);
        }
    }
};

void ring_node_t::gen_render_instances(
    std::vector<wf::scene::render_instance_uptr>& instances,
    wf::scene::damage_callback push_damage, wf::output_t *)
{
    // No shown_on filter: render on every output. The node is a scene-root
    // child, so each output clips the union to its own framebuffer.
    auto self = std::dynamic_pointer_cast<ring_node_t>(shared_from_this());
    instances.push_back(
        std::make_unique<ring_render_instance_t>(self, push_damage));
}
} // namespace

class global_ring_t::impl
{
  public:
    bool all_ws;
    wf::color_t background;
    int gap;
    anim_driver_t driver;
    std::shared_ptr<ring_node_t> node;

    impl(bool all, wf::color_t bg, int g, int ms) :
        all_ws(all), background(bg), gap(std::max(0, g)), driver(ms)
    {}

    ~impl()
    {
        teardown();
    }

    bool start()
    {
        auto views = all_ws ? all_ws_views() : current_ws_views();
        if (views.empty())
        {
            return false;
        }

        node = std::make_shared<ring_node_t>(background);
        node->driver = &driver;
        auto cells = grid_cells(views.size(), outputs_union(), gap);

        // Build every slot's real start rect first; the cell each lands in is
        // decided afterwards by the minimal-movement assignment, not by
        // enumeration order.
        std::vector<slot_t> s;
        for (size_t k = 0; k < views.size() && k < cells.size(); k++)
        {
            auto& pv = views[k];
            slot_t sl;
            sl.view   = pv.view;
            sl.output = pv.view->get_output();
            sl.src    = pv.view->get_transformed_node()->get_bounding_box();
            sl.start  = sl.output ? to_global(sl.output, sl.src) : sl.src;
            sl.buffer.allocate(wf::dimensions(sl.src),
                sl.output ? sl.output->handle->scale : 1.0,
                wf::buffer_allocation_hints_t{.needs_alpha = true});
            sl.need_render = true;
            s.push_back(std::move(sl));
        }

        // Assign cells to slots so total travel is minimised: match each
        // window's real global centre to the nearest free cell centre.
        std::vector<wf::point_t> view_c, cell_c;
        for (auto& sl : s)
        {
            view_c.push_back({sl.start.x + sl.start.width / 2,
                sl.start.y + sl.start.height / 2});
        }

        for (size_t k = 0; k < s.size() && k < cells.size(); k++)
        {
            cell_c.push_back({cells[k].x + cells[k].width / 2,
                cells[k].y + cells[k].height / 2});
        }

        auto assign = min_movement_assignment(view_c, cell_c);
        for (size_t k = 0; k < s.size(); k++)
        {
            s[k].box = fit_in(s[k].src.width, s[k].src.height,
                cells[assign[k]]);
        }

        node->slots = std::move(s);

        // Load the current workspace's wallpaper for the backdrop (drawn behind
        // the thumbnails in render()). Absent wallpaper just leaves the floor.
        if (auto wp = load_current_wallpaper(outputs_union()))
        {
            node->wp_tex   = wp->tex;
            node->wp_cover = wp->cover;
        }

        wf::scene::add_front(wf::get_core().scene(), node);
        driver.open();   // t 0 -> 1: real positions -> grid
        return true;
    }

    void animate_out(std::function<void()> cb)
    {
        if (!node)
        {
            // Nothing was built; complete immediately so the caller tears down.
            if (cb)
            {
                cb();
            }

            return;
        }

        driver.close(std::move(cb));   // t -> 0: thumbnails slide back home
    }

    void teardown()
    {
        if (node)
        {
            wf::scene::remove_child(node);
            node = nullptr;
        }
    }
};

global_ring_t::global_ring_t(bool all_workspaces, wf::color_t background,
    int gap, int anim_ms) :
    priv(std::make_unique<impl>(all_workspaces, background, gap, anim_ms))
{}

global_ring_t::~global_ring_t() = default;

bool global_ring_t::start()
{
    return priv->start();
}

void global_ring_t::animate_out(std::function<void()> on_done)
{
    priv->animate_out(std::move(on_done));
}

bool global_ring_t::closing() const
{
    return priv->driver.closing();
}
} // namespace atlas
