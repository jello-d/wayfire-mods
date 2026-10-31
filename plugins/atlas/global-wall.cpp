// global-wall.cpp: see global-wall.hpp. Derived from Wayfire's
// plugins/common/workspace-wall.cpp @ 746bc7e (v0.10.1).

#include "global-wall.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/region.hpp>
#include <wayfire/render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/workspace-stream.hpp>
#include <wayfire/plugins/common/util.hpp>

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

// Interpolate rect a -> b at t in [0, 1] (t already smoothed by the animation).
wf::geometry_t lerp_geometry(wf::geometry_t a, wf::geometry_t b, double t)
{
    return {
        (int)std::lround(a.x + (b.x - a.x) * t),
        (int)std::lround(a.y + (b.y - a.y) * t),
        (int)std::lround(a.width + (b.width - a.width) * t),
        (int)std::lround(a.height + (b.height - a.height) * t)};
}

// One output's contribution to one workspace cell: a stream of that output's
// wset at workspace (i, j), rendered into its own buffer.
struct tile_t
{
    wf::output_t *output;
    std::shared_ptr<wf::workspace_stream_node_t> stream;
    wf::auxilliary_buffer_t buffer;
    wf::regionf_t damage;
};

class wall_node_t : public wf::scene::node_t
{
  public:
    int gap_size;
    int grid_w = 1;
    int grid_h = 1;
    wf::color_t background;
    wf::geometry_t u{0, 0, 0, 0};       // outputs' union, in layout coords
    std::vector<std::vector<std::vector<tile_t>>> tiles;   // [i][j][output]
    // drag-target tile: (output, cell)
    std::optional<wf::point_t> selection;   // keyboard-selected cell

    // Drag preview: the grabbed window, the cell it is currently aimed at, and
    // the rect tween between cells. preview_driver (owned by global_wall_t)
    // supplies the 0..1 progress; from/to are wall-coord rects.
    wayfire_toplevel_view grabbed = nullptr;
    wf::point_t grab_target{0, 0};
    const anim_driver_t *preview_driver = nullptr;
    wf::geometry_t preview_from{0, 0, 0, 0};
    wf::geometry_t preview_to{0, 0, 0, 0};

    // Open/close animation progress, owned by global_wall_t's shared driver: 0
    // = the current workspace fills every output (looks like the live desktop),
    // 1 = the full grid. `cur` is the workspace the zoom pivots on.
    const anim_driver_t *driver = nullptr;
    wf::point_t cur{0, 0};

    wall_node_t(int gap, wf::color_t bg) :
        node_t(false), gap_size(gap), background(bg)
    {
        u = outputs_union();
        auto outputs = wf::get_core().output_layout->get_outputs();
        if (!outputs.empty() && outputs.front()->wset())
        {
            auto g = outputs.front()->wset()->get_workspace_grid_size();
            grid_w = g.width;
            grid_h = g.height;
            cur    = outputs.front()->wset()->get_current_workspace();
        }

        tiles.resize(grid_w);
        for (int i = 0; i < grid_w; i++)
        {
            tiles[i].resize(grid_h);
            for (int j = 0; j < grid_h; j++)
            {
                for (auto *wo : outputs)
                {
                    tile_t t;
                    t.output = wo;
                    t.stream = std::make_shared<wf::workspace_stream_node_t>(
                        wo, wf::point_t{i, j});
                    auto bbox = t.stream->get_bounding_box();
                    t.buffer.allocate(wf::dimensions(bbox), wo->handle->scale,
                        wf::buffer_allocation_hints_t{.needs_alpha = false});
                    t.damage |= bbox;
                    tiles[i][j].push_back(std::move(t));
                }
            }
        }
    }

    double t() const
    {
        return driver ? driver->progress() : 1.0;
    }

    // Cell (i, j) rectangle in wall coordinates.
    wf::geometry_t cell_rect(int i, int j) const
    {
        return {i * (u.width + gap_size), j * (u.height + gap_size),
            u.width, u.height};
    }

    // The whole grid rectangle in wall coordinates: what the union shows.
    wf::geometry_t wall_rect() const
    {
        return {0, 0,
            grid_w * u.width + (grid_w - 1) * gap_size,
            grid_h * u.height + (grid_h - 1) * gap_size};
    }

    // Output `o`'s sub-rectangle inside cell (i, j), in wall coordinates: the
    // cell origin plus the output's offset within the union.
    wf::geometry_t tile_rect(int i, int j, wf::output_t *o) const
    {
        auto cell = cell_rect(i, j);
        auto og = o->get_layout_geometry();
        return {cell.x + (og.x - u.x), cell.y + (og.y - u.y),
            og.width, og.height};
    }

    // Map a wall-coord box to its on-screen (layout-coord) rect: the same
    // wall->union scaling render() applies, so damage matches what is drawn.
    wf::geometry_t screen_rect(wf::geometry_t b) const
    {
        auto a = wall_rect();
        double sx = (double)u.width / a.width;
        double sy = (double)u.height / a.height;
        int x0 = (int)std::floor(b.x * sx);
        int y0 = (int)std::floor(b.y * sy);
        int x1 = (int)std::ceil((b.x + b.width) * sx);
        int y1 = (int)std::ceil((b.y + b.height) * sy);
        return {u.x + x0, u.y + y0, x1 - x0, y1 - y0};
    }

    // The grabbed view's rectangle in wall coordinates for an ARBITRARY cell:
    // that cell's output tile (the view keeps its own output), offset by the
    // view's bounding box (a current-workspace view's box is output-local, the
    // current viewport at the origin, and its position is the same in every
    // workspace). nullopt when nothing is grabbed or the view is gone.
    std::optional<wf::geometry_t> view_rect_in_cell(wf::point_t cell) const
    {
        if (!grabbed || !grabbed->is_mapped() || !grabbed->get_output())
        {
            return {};
        }

        auto bb = grabbed->get_bounding_box();
        auto tr = tile_rect(cell.x, cell.y, grabbed->get_output());
        return wf::geometry_t{tr.x + bb.x, tr.y + bb.y, bb.width, bb.height};
    }

    // The preview rect right now: the from->to tween at the driver's progress.
    std::optional<wf::geometry_t> preview_rect() const
    {
        if (!grabbed)
        {
            return {};
        }

        double t = preview_driver ? preview_driver->progress() : 1.0;
        return lerp_geometry(preview_from, preview_to, t);
    }

    // Aim the preview at a new cell: tween from wherever it is now to that
    // cell's rect. Returns false (no restart needed) if the cell is unchanged.
    bool set_target(wf::point_t cell)
    {
        if (cell == grab_target)
        {
            return false;
        }

        auto to = view_rect_in_cell(cell);
        if (!to)
        {
            return false;
        }

        preview_from = preview_rect().value_or(*to);
        preview_to   = *to;
        grab_target  = cell;
        return true;
    }

    // Inverse of the render mapping: a global/layout point -> wall coords
    // (whole grid scaled to the union), then to the cell containing it. Returns
    // nullopt for the inter-cell gap or outside the grid.
    std::optional<wf::point_t> cell_at(wf::pointf_t p) const
    {
        if ((u.width <= 0) || (u.height <= 0))
        {
            return {};
        }

        auto a = wall_rect();
        double wx = (p.x - u.x) * (double)a.width / u.width;
        double wy = (p.y - u.y) * (double)a.height / u.height;
        int cw = u.width + gap_size;
        int ch = u.height + gap_size;
        int i  = (int)std::floor(wx / cw);
        int j  = (int)std::floor(wy / ch);
        if ((i < 0) || (i >= grid_w) || (j < 0) || (j >= grid_h))
        {
            return {};
        }

        if ((wx - i * cw > u.width) || (wy - j * ch > u.height))
        {
            return {};  // in the gap between cells
        }

        return wf::point_t{i, j};
    }

    // Where a global point lands: the output under it, the cell, and the
    // point's coords local to that output's region of the cell (0..out size).
    struct hit_t
    {
        wf::output_t *output;
        wf::point_t cell;
        wf::pointf_t local;
    };

    std::optional<hit_t> hit(wf::pointf_t p) const
    {
        auto cell = cell_at(p);
        if (!cell)
        {
            return {};
        }

        auto a = wall_rect();
        double wx = (p.x - u.x) * (double)a.width / u.width;
        double wy = (p.y - u.y) * (double)a.height / u.height;
        double ux = u.x + wx - cell->x * (double)(u.width + gap_size);
        double uy = u.y + wy - cell->y * (double)(u.height + gap_size);
        for (auto *o : wf::get_core().output_layout->get_outputs())
        {
            auto og = o->get_layout_geometry();
            if ((ux >= og.x) && (ux < og.x + og.width) &&
                (uy >= og.y) && (uy < og.y + og.height))
            {
                return hit_t{o, *cell, {ux - og.x, uy - og.y}};
            }
        }

        return {};
    }

    // The topmost mapped toplevel under a global point, with its cell. Queries
    // the hit output at workspace-(i,j)-relative coords (find_output_view_at
    // deliberately bypasses the output's current-workspace clip).
    std::optional<std::pair<wayfire_toplevel_view, wf::point_t>>
    view_at(wf::pointf_t p) const
    {
        auto h = hit(p);
        if (!h)
        {
            return {};
        }

        auto cur = h->output->wset() ?
            h->output->wset()->get_current_workspace() : wf::point_t{0, 0};
        auto sz = h->output->get_screen_size();
        wf::pointf_t q = {
            h->local.x + (double)(h->cell.x - cur.x) * sz.width,
            h->local.y + (double)(h->cell.y - cur.y) * sz.height};
        auto v = wf::find_output_view_at(h->output, q);
        if (v && v->is_mapped())
        {
            return std::make_pair(v, h->cell);
        }

        return {};
    }

    std::optional<std::pair<wf::output_t*, wf::point_t>>
    drop_target(wf::pointf_t p) const
    {
        auto h = hit(p);
        return h ? std::make_optional(std::make_pair(h->output, h->cell))
                 : std::nullopt;
    }

    std::string stringify() const override
    {
        return "atlas-global-wall " + stringify_flags();
    }

    // Spans the whole logical desktop.
    wf::geometry_t get_bounding_box() override
    {
        return u;
    }

    void gen_render_instances(
        std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *shown_on)
        override;
};

class wall_render_instance_t : public wf::scene::render_instance_t
{
    std::shared_ptr<wall_node_t> self;
    wf::scene::damage_callback push_damage;
    // Child render instances, parallel to self->tiles: [i][j][output].
    std::vector<std::vector<std::vector<
        std::vector<wf::scene::render_instance_uptr>>>> child;

  public:
    wall_render_instance_t(std::shared_ptr<wall_node_t> node,
        wf::scene::damage_callback pd) : self(node), push_damage(pd)
    {
        child.resize(self->grid_w);
        for (int i = 0; i < self->grid_w; i++)
        {
            child[i].resize(self->grid_h);
            for (int j = 0; j < self->grid_h; j++)
            {
                child[i][j].resize(self->tiles[i][j].size());
                for (size_t k = 0; k < self->tiles[i][j].size(); k++)
                {
                    auto pdc = [this, i, j, k] (const wf::regionf_t& dmg)
                    {
                        self->tiles[i][j][k].damage |= dmg;
                        // Coarse: any stream change repaints the whole wall.
                        push_damage(self->get_bounding_box());
                    };
                    self->tiles[i][j][k].stream->gen_render_instances(
                        child[i][j][k], pdc, self->tiles[i][j][k].output);
                }
            }
        }
    }

    void schedule_instructions(
        std::vector<wf::scene::render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::regionf_t& damage) override
    {
        // Refresh each tile's buffer from its workspace stream.
        for (int i = 0; i < self->grid_w; i++)
        {
            for (int j = 0; j < self->grid_h; j++)
            {
                for (size_t k = 0; k < self->tiles[i][j].size(); k++)
                {
                    auto& t = self->tiles[i][j][k];
                    if (t.damage.empty())
                    {
                        continue;
                    }

                    wf::render_target_t aux{t.buffer};
                    aux.geometry = t.stream->get_bounding_box();
                    aux.scale    = t.output->handle->scale;

                    wf::render_pass_params_t params;
                    params.instances = &child[i][j][k];
                    params.damage    = t.damage;
                    params.reference_output = t.output;
                    params.target = aux;
                    params.flags  = wf::RPASS_EMIT_SIGNALS;
                    wf::render_pass_t::run(params);

                    t.damage.clear();
                }
            }
        }

        // Translucent overlays must be repainted IN FULL on every frame the
        // wall renders. Partial per-frame damage (a cursor-only frame, say)
        // repaints only a slice of a blended rect, so across the swapchain some
        // buffers hold a complete box and others a stale one: it flickers,
        // worst at the output seams, where the box is one rect split across
        // outputs that paint their slices on independent timing. Opaque tiles
        // are damage-idempotent and need none of this; a blend does. Force each
        // active overlay's whole screen rect into the damage so every rendered
        // buffer gets a complete copy. (Costs nothing when the wall is idle: no
        // frame, no forced damage.)
        if (self->selection)
        {
            damage |= self->screen_rect(self->cell_rect(
                self->selection->x, self->selection->y));
        }

        if (auto pr = self->preview_rect())
        {
            damage |= self->screen_rect(*pr);
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

        // Zoom the source rect from the current workspace's cell (t=0, that
        // one cell fills the union so the wall opens out of the live desktop)
        // to the whole grid (t=1). scale_fbox maps every tile box from A into
        // the union B, so interpolating A alone animates all tiles, selection
        // and highlight washes together.
        // 0.11: geometry_t is float, so the old geometry_to_fbox conversions
        // are gone and scale_fbox became scale_box; add_texture takes a
        // shared_ptr<texture_t> built from the aux buffer via from_aux.
        const double t = self->t();
        auto A = lerp_geometry(
            self->cell_rect(self->cur.x, self->cur.y), self->wall_rect(), t);
        auto B = self->get_bounding_box();
        for (int i = 0; i < self->grid_w; i++)
        {
            for (int j = 0; j < self->grid_h; j++)
            {
                for (size_t k = 0; k < self->tiles[i][j].size(); k++)
                {
                    auto& t  = self->tiles[i][j][k];
                    auto box = self->tile_rect(i, j, t.output);
                    auto render_geometry = wf::scale_box(A, B, box);

                    auto tex = wf::texture_t::from_aux(t.buffer);
                    tex->set_filter_mode(WLR_SCALE_FILTER_BILINEAR);
                    data.pass->add_texture(tex, data.target, render_geometry,
                        data.damage);
                }
            }
        }

        // Keyboard selection: a WHISPER of green over the whole selected cell.
        // The zoom animation already says which workspace is which, so this is
        // ~10% alpha, not a slab: enough to locate the cursor, not enough to
        // shimmer over the live workspace under it. add_rect wants premult
        // colour, so green (0.2, 0.85, 0.35) at 0.1 alpha is those times 0.1.
        if (self->selection)
        {
            auto box = self->cell_rect(
                self->selection->x, self->selection->y);
            data.pass->add_rect({0.02, 0.085, 0.035, 0.1}, data.target,
                wf::scale_box(A, B, box), data.damage);
        }

        // Drag preview: a solid 40% blue over the grabbed window (premultiplied
        // blue (0.2, 0.4, 1.0) at 0.4), at its own position within whichever
        // workspace is the current target, tweened between cells. Drawn last so
        // it reads on top. This is the WHOLE drag cue: the target workspace
        // is NOT washed green; only home keeps its selection tint.
        if (auto pr = self->preview_rect())
        {
            data.pass->add_rect({0.08, 0.16, 0.4, 0.4}, data.target,
                wf::scale_box(A, B, *pr), data.damage);
        }
    }

    void compute_visibility(wf::output_t *output, wf::regionf_t& visible)
        override
    {
        for (int i = 0; i < self->grid_w; i++)
        {
            for (int j = 0; j < self->grid_h; j++)
            {
                for (size_t k = 0; k < self->tiles[i][j].size(); k++)
                {
                    wf::regionf_t r =
                        self->tiles[i][j][k].stream->get_bounding_box();
                    for (auto& ch : child[i][j][k])
                    {
                        ch->compute_visibility(output, r);
                    }
                }
            }
        }
    }
};

void wall_node_t::gen_render_instances(
    std::vector<wf::scene::render_instance_uptr>& instances,
    wf::scene::damage_callback push_damage, wf::output_t *shown_on)
{
    // No shown_on filter: render on every output. The node is a scene-root
    // child, so each output clips the union to its own framebuffer and the grid
    // spans them.
    auto self = std::dynamic_pointer_cast<wall_node_t>(shared_from_this());
    instances.push_back(
        std::make_unique<wall_render_instance_t>(self, push_damage));
}
} // namespace

global_wall_t::global_wall_t(int gap, wf::color_t bg, int anim_ms) :
    gap_size(gap), background(bg), anim_ms(anim_ms), driver(anim_ms)
{}

global_wall_t::~global_wall_t()
{
    stop();
}

void global_wall_t::start()
{
    if (node)
    {
        return;
    }

    auto wn = std::make_shared<wall_node_t>(gap_size, background);
    wn->driver = &driver;
    node = wn;
    wf::scene::add_front(wf::get_core().scene(), node);
    driver.open();   // t 0 -> 1: current workspace zooms out to the grid
}

void global_wall_t::animate_out(std::function<void()> on_done)
{
    if (!node)
    {
        if (on_done)
        {
            on_done();
        }

        return;
    }

    driver.close(std::move(on_done));   // grid zooms back into the current ws
}

bool global_wall_t::closing() const
{
    return driver.closing();
}

void global_wall_t::stop()
{
    if (!node)
    {
        return;
    }

    preview.reset();   // drop the drag-preview frame pump
    wf::scene::remove_child(node);
    node = nullptr;
}

std::optional<wf::point_t> global_wall_t::cell_at(wf::pointf_t global) const
{
    auto wn = std::dynamic_pointer_cast<wall_node_t>(node);
    return wn ? wn->cell_at(global) : std::nullopt;
}

std::optional<std::pair<wayfire_toplevel_view, wf::point_t>>
global_wall_t::view_at(wf::pointf_t global) const
{
    auto wn = std::dynamic_pointer_cast<wall_node_t>(node);
    return wn ? wn->view_at(global) : std::nullopt;
}

std::optional<std::pair<wf::output_t*, wf::point_t>>
global_wall_t::drop_target(wf::pointf_t global) const
{
    auto wn = std::dynamic_pointer_cast<wall_node_t>(node);
    return wn ? wn->drop_target(global) : std::nullopt;
}

void global_wall_t::set_selection(std::optional<wf::point_t> cell)
{
    auto wn = std::dynamic_pointer_cast<wall_node_t>(node);
    if (!wn || (wn->selection == cell))
    {
        return;
    }

    wf::regionf_t dmg;
    if (wn->selection)
    {
        dmg |= wn->screen_rect(
            wn->cell_rect(wn->selection->x, wn->selection->y));
    }

    if (cell)
    {
        dmg |= wn->screen_rect(wn->cell_rect(cell->x, cell->y));
    }

    wn->selection = cell;
    if (!dmg.empty())
    {
        wf::scene::damage_node(node, dmg);
    }
}

void global_wall_t::set_grabbed(wayfire_toplevel_view view)
{
    auto wn = std::dynamic_pointer_cast<wall_node_t>(node);
    if (!wn || (wn->grabbed == view))
    {
        return;
    }

    wf::regionf_t dmg;
    if (auto r = wn->preview_rect())   // the preview leaving
    {
        dmg |= wn->screen_rect(*r);
    }

    wn->grabbed = view;
    if (view)
    {
        // Preview starts static on the window's own (home) cell.
        wn->grab_target = wn->cur;
        auto home = wn->view_rect_in_cell(wn->cur).value_or(
            wf::geometry_t{0, 0, 0, 0});
        wn->preview_from = home;
        wn->preview_to   = home;
        preview = std::make_unique<anim_driver_t>(anim_ms);
        wn->preview_driver = preview.get();
        preview->arm();   // install the frame pump; no motion yet
    }
    else
    {
        wn->preview_driver = nullptr;
        preview.reset();
    }

    if (auto r = wn->preview_rect())   // the preview arriving
    {
        dmg |= wn->screen_rect(*r);
    }

    if (!dmg.empty())
    {
        wf::scene::damage_node(node, dmg);
    }
}

void global_wall_t::set_grab_target(wf::point_t cell)
{
    auto wn = std::dynamic_pointer_cast<wall_node_t>(node);
    if (!wn || !wn->grabbed)
    {
        return;
    }

    if (wn->set_target(cell) && preview)
    {
        preview->restart();   // tween the blue window to the new cell
    }
}

wf::point_t global_wall_t::grab_target() const
{
    auto wn = std::dynamic_pointer_cast<wall_node_t>(node);
    return wn ? wn->grab_target : wf::point_t{0, 0};
}
} // namespace atlas
