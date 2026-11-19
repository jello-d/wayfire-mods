// toplevel-capture: ext-foreign-toplevel-list-v1 plus per-WINDOW image
// capture (ext-image-capture-source-v1's toplevel source), aware of
// security-context-v1 sandboxes, so a sandboxed app can share ITS OWN
// windows and nothing else (severance SV9).
//
// LISTS. wlroots sends every handle of a list to every client that binds it,
// so one list cannot be filtered per client. There is therefore one PERSONAL
// list (every window) shown only to clients outside any security context,
// and one list per sandbox INSTANCE (only that instance's windows) shown only
// to clients of that instance. An instance's list is created when its context
// is COMMITTED, before any of its clients can connect, so a portal that binds
// at startup already finds it. Any other ext_foreign_toplevel_list_v1 global
// (upstream's `ext-toplevel` plugin, if someone loads both) is hidden from
// sandboxed clients, which fails closed.
//
// CAPTURE. A request is accepted when the window belongs to the requester's
// own instance, or when the requester is not sandboxed at all; anything else
// gets an inert source. The lists already make a foreign window unreachable;
// the check is the second wall. A source renders the window, CROPPED TO ITS
// GEOMETRY (no shadow, no enclave-mark frame), into an offscreen buffer and
// hands that to the copy-capture session.
//
// SCREENS. The real whole-output capture (wayfire core's) never reaches a
// sandbox: the allowlist does not name it and this plugin's filter hides it
// too. Instead each instance gets its OWN ext_output_image_capture_source_
// manager_v1 global, which renders an output with ONLY that instance's
// windows over black: decorations included, the enclave mark not, an enclave
// window drawn whole where a personal one covers it, and the pointer only
// while the topmost window under it is the instance's. The global is scoped
// through security-context.hpp's instance_scope_t, so the security-context
// plugin shows it to that instance alone whatever the allowlist says, and a
// compositor without this plugin offers an enclave no screen at all.
//
// THE REVERSE, for a person. Core's screen capture global is hidden from
// every client, and unsandboxed ones get this plugin's PERSONAL manager
// instead: the output as it is shown, with every enclave view covered by an
// opaque black box (veil_node_t, in front of the view's own drawing, so a
// personal window above it still draws over it). A personal client asking
// for an enclave WINDOW gets black frames at its size. wlr-screencopy and
// export-dmabuf are NOT covered: they copy the composited frame itself, and
// that is deliberate (user's call, 2026-11-15): a share reaches other people
// live, while a screenshot is a local file its owner can edit or discard.
//
// Replaces upstream `ext-toplevel`; do not load both.

#include <wayfire/core.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/util.hpp>
#include <wayfire/view.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/plugins/common/security-context.hpp>
#include <wayfire/plugins/common/toplevel-identifier.hpp>
#include <drm_fourcc.h>
#include <cstring>
#include <map>
#include <memory>
#include <time.h>

extern "C"
{
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include "ext-image-capture-source-v1-protocol.h"
}
#include "viewer-pass.hpp"

namespace sc = wf::security_context;

// Above zero while a PERSONAL screen share is collecting or drawing its
// frame: enclave windows then draw as black (veil_node_t, below). The
// output's own draw lists are made at other times, so the screen is untouched.
static int personal_pass = 0;

class view_source_t;

namespace
{
const char *const RECORD_DATA = "toplevel-capture";

// Standard-layout wrappers, so a wlroots base pointer maps back to its owner
// with a plain cast from the first member.
struct source_wrap_t
{
    wlr_ext_image_capture_source_v1 base;
    view_source_t *self;
};

struct frame_event_t
{
    wlr_ext_image_capture_source_v1_frame_event base;
    wlr_buffer *buffer;
    timespec when;
};

// The window geometry, in the coordinates its root node renders in (the
// root's PARENT), so a render target set to it crops to the window.
wf::geometry_t window_box(wayfire_toplevel_view view)
{
    auto root   = view->get_surface_root_node();
    auto origin = root->parent() ? root->parent()->to_global({0, 0}) :
        wf::pointf_t{0, 0};
    auto g = view->get_geometry();
    return {g.x - origin.x, g.y - origin.y, g.width, g.height};
}
}

// One capture source per window, shared by every session on it.
class view_source_t
{
  public:
    source_wrap_t wrap;
    wayfire_toplevel_view view;
    // An enclave window asked for by a personal client: frames are black at
    // the window's size, and the window's own content is never drawn.
    bool veiled = false;
    // Frames are rendered straight into this swapchain's buffers, which are
    // what the copy-capture session copies from (GPU to GPU for a dmabuf
    // client). Recreated at each size change.
    wlr_swapchain *swapchain = nullptr;
    std::vector<wf::scene::render_instance_uptr> instances;
    int started   = 0;
    bool dirty    = true;
    bool wanted   = false;
    bool rendered = false;
    wf::wl_idle_call idle;

    wf::signal::connection_t<wf::scene::node_update_signal> on_update =
        [=] (wf::scene::node_update_signal*) { regen(); };

    // The source outlives sessions, so its size must follow the window NOW:
    // a session opened after a resize would otherwise be told the old size
    // and fail its first frame (measured, before this listener existed).
    wf::signal::connection_t<wf::view_geometry_changed_signal> on_geometry =
        [=] (wf::view_geometry_changed_signal*)
    {
        if (update_constraints())
        {
            mark_dirty();
        }
    };

    view_source_t(wayfire_toplevel_view view, bool veiled);
    ~view_source_t();

    // Render instances kept alive for their DAMAGE callback: a window keeps
    // producing frames while it is on another workspace, or covered.
    void regen()
    {
        instances.clear();
        if (!veiled)
        {
            view->get_surface_root_node()->gen_render_instances(instances,
                [=] (const wf::regionf_t&) { mark_dirty(); },
                view->get_output());
        }

        mark_dirty();
    }

    void mark_dirty()
    {
        dirty = true;
        schedule();
    }

    void schedule()
    {
        if ((started > 0) && wanted && dirty)
        {
            idle.run_once([=] { produce_frame(); });
        }
    }

    bool update_constraints();
    void produce_frame();
    void keep_client_drawing(const timespec& now);
};

static source_wrap_t *wrap_of(wlr_ext_image_capture_source_v1 *base)
{
    return reinterpret_cast<source_wrap_t*>(base);
}

static void source_start(wlr_ext_image_capture_source_v1 *base, bool)
{
    auto self = wrap_of(base)->self;
    if (self->started++ == 0)
    {
        self->regen();
    }
}

static void source_stop(wlr_ext_image_capture_source_v1 *base)
{
    auto self = wrap_of(base)->self;
    if (--self->started == 0)
    {
        self->instances.clear();
        self->wanted = false;
    }
}

static void source_request_frame(wlr_ext_image_capture_source_v1 *base,
    bool schedule_frame)
{
    auto self = wrap_of(base)->self;
    self->wanted = true;
    // The first frame of a session has to come even without new damage.
    if (schedule_frame && !self->rendered)
    {
        self->dirty = true;
    }

    self->schedule();
}

static void source_copy_frame(wlr_ext_image_capture_source_v1 *base,
    wlr_ext_image_copy_capture_frame_v1 *frame,
    wlr_ext_image_capture_source_v1_frame_event *base_event)
{
    auto event = reinterpret_cast<frame_event_t*>(base_event);
    if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, event->buffer,
        wf::get_core().renderer))
    {
        wlr_ext_image_copy_capture_frame_v1_ready(frame,
            WL_OUTPUT_TRANSFORM_NORMAL, &event->when);
    }
}

static void refused_start(wlr_ext_image_capture_source_v1*, bool)
{}
static void refused_stop(wlr_ext_image_capture_source_v1*)
{}
static void refused_request_frame(wlr_ext_image_capture_source_v1*, bool)
{}
static void refused_copy_frame(wlr_ext_image_capture_source_v1*,
    wlr_ext_image_copy_capture_frame_v1*,
    wlr_ext_image_capture_source_v1_frame_event*)
{}

// Never started: the source it backs is destroyed as soon as it is handed out.
static const wlr_ext_image_capture_source_v1_interface refused_impl = {
    .start = refused_start,
    .stop  = refused_stop,
    .request_frame = refused_request_frame,
    .copy_frame    = refused_copy_frame,
    .get_pointer_cursor = nullptr,
};

static const wlr_ext_image_capture_source_v1_interface source_impl = {
    .start = source_start,
    .stop  = source_stop,
    .request_frame = source_request_frame,
    .copy_frame    = source_copy_frame,
    .get_pointer_cursor = nullptr,
};

view_source_t::view_source_t(wayfire_toplevel_view view, bool veiled) :
    view(view), veiled(veiled)
{
    wrap.self = this;
    wlr_ext_image_capture_source_v1_init(&wrap.base, &source_impl);
    view->get_surface_root_node()->connect(&on_update);
    view->connect(&on_geometry);
    update_constraints();
}

view_source_t::~view_source_t()
{
    on_update.disconnect();
    on_geometry.disconnect();
    instances.clear();
    wlr_ext_image_capture_source_v1_finish(&wrap.base);
    if (swapchain)
    {
        wlr_swapchain_destroy(swapchain);
    }
}

// Size from the window geometry at the output's scale. The format comes from
// a swapchain made at that size in the OUTPUT's own format and modifiers, and
// wlroots derives the constraints from it: shm, plus dmabuf (that format and
// ARGB/XRGB fallbacks) where the allocator and renderer can do dmabuf. That
// keeps wlroots' render-format query, private in 0.20, on wlroots' side. On
// a software renderer it simply offers shm. True when the constraints changed.
bool view_source_t::update_constraints()
{
    auto out   = view->get_output();
    float scale = out ? out->handle->scale : 1.0;
    auto box   = window_box(view);
    uint32_t w = std::max(1, (int)std::round(box.width * scale));
    uint32_t h = std::max(1, (int)std::round(box.height * scale));
    if (swapchain && (wrap.base.width == w) && (wrap.base.height == h))
    {
        return false;
    }

    // No output swapchain yet: ARGB8888 with an implicit modifier.
    uint64_t implicit = DRM_FORMAT_MOD_INVALID;
    wlr_drm_format fallback{DRM_FORMAT_ARGB8888, 1, 1, &implicit};
    const wlr_drm_format *fmt = (out && out->handle->swapchain) ?
        &out->handle->swapchain->format : &fallback;
    auto& core = wf::get_core();
    auto next  = wlr_swapchain_create(core.allocator, w, h, fmt);
    if (!next)
    {
        LOGE("toplevel-capture: no ", w, "x", h, " swapchain for view ",
            view->get_id(), "; this window cannot be captured");
        return false;
    }

    if (swapchain)
    {
        wlr_swapchain_destroy(swapchain);
    }

    swapchain = next;
    wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&wrap.base,
        swapchain, core.renderer);
    wl_signal_emit_mutable(&wrap.base.events.constraints_update, nullptr);
    return true;
}

void view_source_t::produce_frame()
{
    if (!view->is_mapped() || !view->get_output() || (started == 0))
    {
        return;
    }

    update_constraints();
    wlr_buffer *buffer = swapchain ? wlr_swapchain_acquire(swapchain) : nullptr;
    if (!buffer)
    {
        return;
    }

    auto box = window_box(view);
    float scale = view->get_output()->handle->scale;
    wf::render_target_t target{wf::render_buffer_t(buffer,
        {(int)wrap.base.width, (int)wrap.base.height})};
    target.geometry = box;
    target.scale    = scale;
    wf::render_pass_params_t params;
    params.background_color = veiled ? wf::color_t{0, 0, 0, 1} :
        wf::color_t{0, 0, 0, 0};
    params.damage    = box;
    params.target    = target;
    params.instances = &instances;
    params.flags     = wf::RPASS_CLEAR_BACKGROUND;
    params.reference_output = view->get_output();
    wf::render_pass_t::run(params);

    pixman_region32_t damage;
    pixman_region32_init_rect(&damage, 0, 0, wrap.base.width, wrap.base.height);
    frame_event_t event{};
    event.base.damage = &damage;
    event.buffer = buffer;
    clock_gettime(CLOCK_MONOTONIC, &event.when);
    dirty    = false;
    wanted   = false;
    rendered = true;
    // Sessions copy synchronously inside this emit, so the buffer can go
    // back to the swapchain right after.
    wl_signal_emit_mutable(&wrap.base.events.frame, &event.base);
    wlr_buffer_unlock(buffer);
    pixman_region32_fini(&damage);
    keep_client_drawing(event.when);
}

// A client redraws only when its frame callbacks fire, and wayfire fires them
// only for surfaces visible on an output. So a window shared while covered,
// off-screen or on another workspace would stop painting after one frame
// (measured: an off-screen window streamed 1 frame, then nothing). Fire them
// after each captured frame, so the client paints at the rate the capture
// consumes. Harmless when it is also visible: a callback fires once, so the
// rate stays capped by the capture.
void view_source_t::keep_client_drawing(const timespec& now)
{
    if (auto surface = view->get_wlr_surface())
    {
        wlr_surface_for_each_surface(surface,
            [] (wlr_surface *s, int, int, void *data)
        {
            wlr_surface_send_frame_done(s, (const timespec*)data);
        }, (void*)&now);
    }
}

// ---------------------------------------------------------------------------
// SCREEN capture for an enclave: one output, drawn with ONLY that instance's
// windows, over black. Nothing personal is ever in the draw list, so nothing
// personal can reach the frame: not the panel, the wallpaper, a notification,
// or a personal window, and not even where one sits (an enclave window a
// personal one covers is drawn whole, user's call 2026-11-15). Decorations
// come with each window (they are in its subtree); the enclave mark does not
// (viewer-pass.hpp).
class screen_source_t;

struct screen_wrap_t
{
    wlr_ext_image_capture_source_v1 base;
    screen_source_t *self;
};

class screen_source_t
{
  public:
    screen_wrap_t wrap;
    wf::output_t *output;
    // The instance this screen is drawn for, or empty for the PERSONAL
    // screen: the whole output as it is shown, with every enclave window
    // drawn black where it sits (veil_node_t).
    std::string instance;
    wlr_swapchain *swapchain = nullptr;
    // One draw list per window, each in its own parent's coordinates; the
    // personal screen has one, the whole scene, in layout coordinates.
    struct layer_t
    {
        wf::scene::node_ptr root;
        std::vector<wf::scene::render_instance_uptr> instances;
        bool layout = false;
    };
    std::vector<layer_t> layers;
    int started   = 0;
    int with_cursors = 0;    // sessions that asked for the pointer drawn in
    bool dirty    = true;
    bool wanted   = false;
    bool rendered = false;
    wf::wl_idle_call idle;
    wf::shared_data::ref_ptr_t<wf::enclave_capture::viewer_pass_t> viewer_pass;

    // Any change to what is in the scene (a window mapped, raised, moved to
    // another workspace) can change what this screen shows.
    wf::signal::connection_t<wf::scene::root_node_update_signal> on_scene =
        [=] (wf::scene::root_node_update_signal*) { mark_dirty(); };
    wf::signal::connection_t<wf::workspace_changed_signal> on_workspace =
        [=] (wf::workspace_changed_signal*) { mark_dirty(); };
    // The pointer moved: only a share that draws it has a new frame to make.
    wf::signal::connection_t<
        wf::post_input_event_signal<wlr_pointer_motion_event>>
    on_motion = [=] (auto*) { if (with_cursors > 0) { mark_dirty(); } };
    wf::signal::connection_t<
        wf::post_input_event_signal<wlr_pointer_motion_absolute_event>>
    on_motion_abs = [=] (auto*) { if (with_cursors > 0) { mark_dirty(); } };

    screen_source_t(wf::output_t *output, std::string instance);
    ~screen_source_t();

    bool belongs(wayfire_view view) const
    {
        auto ctx = sc::lookup_client(view->get_client());
        return ctx && (ctx->instance_id == instance) && view->is_mapped() &&
               (view->get_output() == output);
    }

    // The draw lists, bottom to top: this instance's windows in stacking
    // order (a child dialog comes with its parent's subtree, so only roots),
    // then its top-level popups, which live in the output's unmanaged layer.
    bool personal() const
    {
        return instance.empty();
    }

    void regen()
    {
        layers.clear();
        viewer_pass->depth++;
        if (personal())
        {
            personal_pass++;
            layer_t layer;
            layer.layout = true;
            wf::get_core().scene()->gen_render_instances(layer.instances,
                [=] (const wf::regionf_t&) { mark_dirty(); }, output);
            layers.push_back(std::move(layer));
            personal_pass--;
            viewer_pass->depth--;
            return;
        }

        auto add = [&] (wayfire_view view)
        {
            layer_t layer;
            layer.root = view->get_root_node();
            layer.root->gen_render_instances(layer.instances,
                [=] (const wf::regionf_t&) { mark_dirty(); }, output);
            layers.push_back(std::move(layer));
        };
        auto views = output->wset()->get_views(wf::WSET_MAPPED_ONLY |
            wf::WSET_EXCLUDE_MINIMIZED | wf::WSET_SORT_STACKING);
        for (auto it = views.rbegin(); it != views.rend(); ++it)
        {
            if (!(*it)->parent && belongs(*it))
            {
                add(*it);
            }
        }

        auto unmanaged = output->node_for_layer(wf::scene::layer::UNMANAGED);
        for (auto& view : wf::get_core().get_all_views())
        {
            if (!wf::toplevel_cast(view) && belongs(view) &&
                (view->get_root_node()->parent() == unmanaged.get()))
            {
                add(view);
            }
        }

        viewer_pass->depth--;
    }

    void mark_dirty()
    {
        dirty = true;
        schedule();
    }

    void schedule()
    {
        if ((started > 0) && wanted && dirty)
        {
            idle.run_once([=] { produce_frame(); });
        }
    }

    bool update_constraints();
    void produce_frame();
    void draw_pointer(wlr_buffer *buffer);
};

static screen_wrap_t *screen_wrap_of(wlr_ext_image_capture_source_v1 *base)
{
    return reinterpret_cast<screen_wrap_t*>(base);
}

static void screen_start(wlr_ext_image_capture_source_v1 *base,
    bool with_cursors)
{
    auto self = screen_wrap_of(base)->self;
    self->with_cursors += with_cursors ? 1 : 0;
    if (self->started++ == 0)
    {
        self->mark_dirty();
    }
}

static void screen_stop(wlr_ext_image_capture_source_v1 *base)
{
    auto self = screen_wrap_of(base)->self;
    if (--self->started == 0)
    {
        self->with_cursors = 0;
        self->layers.clear();
        self->wanted = false;
    }
}

static void screen_request_frame(wlr_ext_image_capture_source_v1 *base,
    bool schedule_frame)
{
    auto self = screen_wrap_of(base)->self;
    self->wanted = true;
    if (schedule_frame && !self->rendered)
    {
        self->dirty = true;
    }

    self->schedule();
}

static const wlr_ext_image_capture_source_v1_interface screen_impl = {
    .start = screen_start,
    .stop  = screen_stop,
    .request_frame = screen_request_frame,
    .copy_frame    = source_copy_frame,
    .get_pointer_cursor = nullptr,
};

screen_source_t::screen_source_t(wf::output_t *output, std::string instance) :
    output(output), instance(std::move(instance))
{
    wrap.self = this;
    wlr_ext_image_capture_source_v1_init(&wrap.base, &screen_impl);
    wf::get_core().scene()->connect(&on_scene);
    output->connect(&on_workspace);
    wf::get_core().connect(&on_motion);
    wf::get_core().connect(&on_motion_abs);
    update_constraints();
}

screen_source_t::~screen_source_t()
{
    on_scene.disconnect();
    on_workspace.disconnect();
    on_motion.disconnect();
    on_motion_abs.disconnect();
    layers.clear();
    wlr_ext_image_capture_source_v1_finish(&wrap.base);
    if (swapchain)
    {
        wlr_swapchain_destroy(swapchain);
    }
}

// The output's layout size at its scale, in its own format (as for a window).
bool screen_source_t::update_constraints()
{
    auto og = output->get_relative_geometry();
    float scale = output->handle->scale;
    uint32_t w = std::max(1, (int)std::round(og.width * scale));
    uint32_t h = std::max(1, (int)std::round(og.height * scale));
    if (swapchain && (wrap.base.width == w) && (wrap.base.height == h))
    {
        return false;
    }

    uint64_t implicit = DRM_FORMAT_MOD_INVALID;
    wlr_drm_format fallback{DRM_FORMAT_ARGB8888, 1, 1, &implicit};
    const wlr_drm_format *fmt = output->handle->swapchain ?
        &output->handle->swapchain->format : &fallback;
    auto& core = wf::get_core();
    auto next  = wlr_swapchain_create(core.allocator, w, h, fmt);
    if (!next)
    {
        LOGE("toplevel-capture: no ", w, "x", h, " swapchain for output ",
            output->to_string(), "; this screen cannot be captured");
        return false;
    }

    if (swapchain)
    {
        wlr_swapchain_destroy(swapchain);
    }

    swapchain = next;
    wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&wrap.base,
        swapchain, core.renderer);
    wl_signal_emit_mutable(&wrap.base.events.constraints_update, nullptr);
    return true;
}

void screen_source_t::produce_frame()
{
    if (started == 0)
    {
        return;
    }

    update_constraints();
    regen();
    wlr_buffer *buffer = swapchain ? wlr_swapchain_acquire(swapchain) : nullptr;
    if (!buffer)
    {
        return;
    }

    auto og = output->get_relative_geometry();
    float scale = output->handle->scale;
    bool first  = true;
    // The output's own origin, in the same space a window's parent reports
    // in: windows are laid out OUTPUT-LOCAL (measured: a window on an output
    // at layout x=1280 has its parent at 0,0), so a box taken from the layout
    // geometry missed every window on any output away from the origin. Taken
    // relative to the output's node, it is right whichever the space is.
    auto ref = output->node_for_layer(wf::scene::layer::WORKSPACE)->parent();
    auto out_origin = ref ? ref->to_global({0, 0}) : wf::pointf_t{0, 0};
    // Black first, even with no window to draw: an empty screen is a valid
    // frame, and a stale buffer would show the previous one.
    auto pass = [&] (std::vector<wf::scene::render_instance_uptr> *instances,
                     wf::scene::node_ptr root, bool layout)
    {
        auto origin = (root && root->parent()) ?
            root->parent()->to_global({0, 0}) : out_origin;
        wf::geometry_t box = layout ? output->get_layout_geometry() :
            wf::geometry_t{og.x + (int)(out_origin.x - origin.x),
                og.y + (int)(out_origin.y - origin.y), og.width, og.height};
        wf::render_target_t target{wf::render_buffer_t(buffer,
            {(int)wrap.base.width, (int)wrap.base.height})};
        target.geometry = box;
        target.scale    = scale;
        std::vector<wf::scene::render_instance_uptr> none;
        wf::render_pass_params_t params;
        params.background_color = {0, 0, 0, 1};
        params.damage    = box;
        params.target    = target;
        params.instances = instances ? instances : &none;
        params.flags     = first ? wf::RPASS_CLEAR_BACKGROUND : 0;
        params.reference_output = output;
        wf::render_pass_t::run(params);
        first = false;
    };
    // The veils draw only while this is raised, so it covers the drawing as
    // well as regen(): an instance made outside it would draw the window.
    personal_pass += personal() ? 1 : 0;
    pass(nullptr, nullptr, false);
    for (auto& layer : layers)
    {
        pass(&layer.instances, layer.root, layer.layout);
    }

    personal_pass -= personal() ? 1 : 0;


    if (with_cursors > 0)
    {
        draw_pointer(buffer);
    }

    pixman_region32_t damage;
    pixman_region32_init_rect(&damage, 0, 0, wrap.base.width, wrap.base.height);
    frame_event_t event{};
    event.base.damage = &damage;
    event.buffer = buffer;
    clock_gettime(CLOCK_MONOTONIC, &event.when);
    dirty    = false;
    wanted   = false;
    rendered = true;
    wl_signal_emit_mutable(&wrap.base.events.frame, &event.base);
    wlr_buffer_unlock(buffer);
    pixman_region32_fini(&damage);
}

// The pointer, ONLY when the topmost window under it is this instance's
// (user's call, 2026-11-15): over a personal window, or over nothing of the
// enclave's, the share shows no pointer, so it cannot be followed around
// personal space. Drawn from the output's own cursor image, upright: the
// share is in the output's LOGICAL orientation, and wlroots keeps the cursor
// hotspot in the output's buffer (rotated) space, so it is turned back.
void screen_source_t::draw_pointer(wlr_buffer *buffer)
{
    auto at = wf::get_core().get_cursor_position();
    auto lg = output->get_layout_geometry();
    if ((at.x < lg.x) || (at.y < lg.y) || (at.x >= lg.x + lg.width) ||
        (at.y >= lg.y + lg.height))
    {
        return;
    }

    // The personal screen draws it wherever it is: it is that person's own.
    auto view = wf::get_core().get_view_at(at);
    if (!personal() && (!view || !belongs(view)))
    {
        return;
    }

    float scale = output->handle->scale;
    wlr_output_cursor *cursor;
    wl_list_for_each(cursor, &output->handle->cursors, link)
    {
        if (!cursor->enabled || !cursor->visible || !cursor->texture)
        {
            continue;
        }

        int w = cursor->texture->width, h = cursor->texture->height;
        // The hotspot back from the output's buffer space to the image's.
        wlr_box hs{cursor->hotspot_x, cursor->hotspot_y, 0, 0};
        auto inv = wlr_output_transform_invert(cursor->transform);
        int tw = (cursor->transform % 2) ? h : w;
        int th = (cursor->transform % 2) ? w : h;
        wlr_box_transform(&hs, &hs, inv, tw, th);
        wlr_box dst{(int)std::round((at.x - lg.x) * scale) - hs.x,
            (int)std::round((at.y - lg.y) * scale) - hs.y, w, h};
        auto pass = wlr_renderer_begin_buffer_pass(wf::get_core().renderer,
            buffer, nullptr);
        if (!pass)
        {
            return;
        }

        wlr_render_texture_options opts{};
        opts.texture = cursor->texture;
        opts.dst_box = dst;
        opts.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED;
        wlr_render_pass_add_texture(pass, &opts);
        wlr_render_pass_submit(pass);
        return;
    }
}

// The per-instance ext_output_image_capture_source_manager_v1 global. Its
// user data is set BEFORE the global exists, so the instance it serves is
// known while it is first advertised (security-context.hpp, instance_scope).
struct screen_manager_t
{
    std::string instance;
    wl_global *global = nullptr;
    std::function<screen_source_t*(wf::output_t*)> source_for;
};

static void screen_manager_destroy(wl_client*, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static void screen_manager_create_source(wl_client *client,
    wl_resource *resource, uint32_t new_id, wl_resource *output_resource)
{
    auto mgr = (screen_manager_t*)wl_resource_get_user_data(resource);
    auto wlr = wlr_output_from_resource(output_resource);
    auto requester = sc::lookup_client(client);
    auto out = wlr ? wf::get_core().output_layout->find_output(wlr) : nullptr;
    // The filter shows an instance's global to that instance only, and the
    // personal one (empty instance) to unsandboxed clients only; this is the
    // second wall. A refused or vanished output gets an inert source.
    bool mine = mgr && (mgr->instance.empty() ? !requester :
        (requester && (requester->instance_id == mgr->instance)));
    screen_source_t *source = (mine && out) ? mgr->source_for(out) : nullptr;
    if (!source)
    {
        LOGW("toplevel-capture: refused a screen capture request",
            requester ? " from instance '" + requester->instance_id + "'" : "");
        wlr_ext_image_capture_source_v1 refused;
        wlr_ext_image_capture_source_v1_init(&refused, &refused_impl);
        wlr_ext_image_capture_source_v1_create_resource(&refused, client,
            new_id);
        wlr_ext_image_capture_source_v1_finish(&refused);
        return;
    }

    wlr_ext_image_capture_source_v1_create_resource(&source->wrap.base, client,
        new_id);
}

static const struct ext_output_image_capture_source_manager_v1_interface
    screen_manager_impl = {
    .create_source = screen_manager_create_source,
    .destroy = screen_manager_destroy,
};

static void screen_manager_bind(wl_client *client, void *data, uint32_t version,
    uint32_t id)
{
    auto resource = wl_resource_create(client,
        &ext_output_image_capture_source_manager_v1_interface, version, id);
    if (!resource)
    {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource, &screen_manager_impl, data,
        nullptr);
}

// The VEIL over an enclave view: a node in front of everything the view
// draws, which draws an opaque black box over it in a PERSONAL screen share
// (personal_pass) and nothing anywhere else, the output included. So a
// personal share shows where an enclave window is and none of what is in it:
// its content, decorations and shadow alike, and anything personal stacked
// above it still draws above the box. A popup gets its own, since it is a
// view of its own.
class veil_node_t : public wf::scene::node_t
{
    std::weak_ptr<wf::view_interface_t> view;

  public:
    explicit veil_node_t(wayfire_view view) : node_t(false)
    {
        this->view = view->shared_from_this();
    }

    std::string stringify() const override
    {
        return "toplevel-capture veil";
    }

    // In the coordinates of the surface root, our parent. A window: its whole
    // geometry (decorations and any drawn shadow). Anything else: what its
    // other children cover.
    wf::geometry_t get_bounding_box() override
    {
        auto v = view.lock();
        if (!v || !v->is_mapped())
        {
            return {0, 0, 0, 0};
        }

        if (auto toplevel = wf::toplevel_cast(wayfire_view{v.get()}))
        {
            auto g = toplevel->get_geometry();
            auto o = v->get_surface_root_node()->to_global({0, 0});
            return wf::geometry_t{g.x - o.x, g.y - o.y, g.width, g.height};
        }

        wf::regionf_t all;
        if (auto p = parent())
        {
            for (auto& child : p->get_children())
            {
                if (child.get() != this)
                {
                    all |= child->get_bounding_box();
                }
            }
        }

        if (all.empty())
        {
            return {0, 0, 0, 0};
        }

        auto e = all.get_extents();
        return {e.x1, e.y1, e.x2 - e.x1, e.y2 - e.y1};
    }

    class render_instance_t : public wf::scene::render_instance_t
    {
        std::shared_ptr<veil_node_t> self;

      public:
        explicit render_instance_t(veil_node_t *self)
        {
            this->self = std::dynamic_pointer_cast<veil_node_t>(
                self->shared_from_this());
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
            data.pass->add_rect({0, 0, 0, 1}, data.target,
                self->get_bounding_box(), data.damage);
        }
    };

    void gen_render_instances(
        std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback, wf::output_t*) override
    {
        if (personal_pass > 0)
        {
            instances.push_back(std::make_unique<render_instance_t>(this));
        }
    }
};

struct veil_data_t : public wf::custom_data_t
{
    std::shared_ptr<veil_node_t> node;
};

const char *const VEIL_DATA = "toplevel-capture-veil";

// Per window: its handles in the lists it belongs to, and its source.
struct toplevel_record_t : public wf::custom_data_t
{
    wayfire_toplevel_view view;
    std::optional<sc::context_info_t> ctx;
    wlr_ext_foreign_toplevel_handle_v1 *personal = nullptr;
    wlr_ext_foreign_toplevel_handle_v1 *enclave  = nullptr;
    std::unique_ptr<view_source_t> source;
    // For a personal client asking for an enclave window: black frames.
    std::unique_ptr<view_source_t> veiled;

    ~toplevel_record_t()
    {
        source.reset();
        veiled.reset();
        for (auto h : {personal, enclave})
        {
            if (h)
            {
                wlr_ext_foreign_toplevel_handle_v1_destroy(h);
            }
        }
    }

    void update_state()
    {
        std::string title = view->get_title(), app_id = view->get_app_id();
        wlr_ext_foreign_toplevel_handle_v1_state state{title.c_str(),
            app_id.c_str()};
        for (auto h : {personal, enclave})
        {
            if (h)
            {
                wlr_ext_foreign_toplevel_handle_v1_update_state(h, &state);
            }
        }
    }
};

class wayfire_toplevel_capture : public wf::plugin_interface_t
{
    static constexpr uint32_t LIST_VERSION = 1;
    wlr_ext_foreign_toplevel_list_v1 *personal = nullptr;
    std::map<std::string, wlr_ext_foreign_toplevel_list_v1*> enclaves;
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *manager = nullptr;
    std::unique_ptr<wf::wayland_global_filter_t> filter;
    wf::shared_data::ref_ptr_t<sc::registry_t> registry;
    wf::shared_data::ref_ptr_t<sc::instance_scope_t> scope_data;
    wf::wl_listener_wrapper on_new_request;
    // Screen capture, per instance: the manager global, and a source per
    // output, shared by every session on it.
    std::map<std::string, std::unique_ptr<screen_manager_t>> screen_managers;
    std::map<std::pair<std::string, wf::output_t*>,
        std::unique_ptr<screen_source_t>> screens;
    // The PERSONAL screen manager, shown to unsandboxed clients in place of
    // wayfire core's, which is hidden from everyone: a personal screen share
    // draws enclave windows black. Kept out of screen_managers, which holds
    // the instance-scoped ones only.
    std::unique_ptr<screen_manager_t> personal_screens;

    screen_source_t *screen_for(const std::string& instance, wf::output_t *out)
    {
        auto& slot = screens[{instance, out}];
        if (!slot)
        {
            slot = std::make_unique<screen_source_t>(out, instance);
        }

        return slot.get();
    }

    // Registered with the sandbox registry BEFORE the global exists, keyed by
    // the user data the global is created with, so the security-context
    // filter shows it to this instance only from its very first advertisement.
    void screen_manager(const std::string& instance)
    {
        if (screen_managers.count(instance))
        {
            return;
        }

        auto mgr = std::make_unique<screen_manager_t>();
        mgr->instance   = instance;
        mgr->source_for = [=] (wf::output_t *out)
        {
            return screen_for(instance, out);
        };
        auto raw = mgr.get();
        screen_managers[instance] = std::move(mgr);
        raw->global = wl_global_create(wf::get_core().display,
            &ext_output_image_capture_source_manager_v1_interface, 1, raw,
            screen_manager_bind);
        LOGI("toplevel-capture: screen capture for instance '", instance, "'");
    }

    std::optional<std::string> screen_owner(const wl_global *global)
    {
        if (wl_global_get_interface(global) !=
            &ext_output_image_capture_source_manager_v1_interface)
        {
            return std::nullopt;
        }

        auto data = wl_global_get_user_data(global);
        for (auto& [instance, mgr] : screen_managers)
        {
            if (mgr.get() == data)
            {
                return instance;
            }
        }

        return std::nullopt;
    }

    wf::signal::connection_t<wf::output_removed_signal> on_output_removed =
        [=] (wf::output_removed_signal *ev)
    {
        for (auto it = screens.begin(); it != screens.end();)
        {
            it = (it->first.second == ev->output) ? screens.erase(it) :
                std::next(it);
        }
    };

    // Create, THEN insert: creating the global advertises it at once, which
    // runs global_visible() for every connected client while we are still
    // inside wlr_..._create(), so the map must never hold a half-made entry.
    wlr_ext_foreign_toplevel_list_v1 *enclave_list(const std::string& instance)
    {
        auto it = enclaves.find(instance);
        if (it != enclaves.end())
        {
            return it->second;
        }

        auto list = wlr_ext_foreign_toplevel_list_v1_create(
            wf::get_core().display, LIST_VERSION);
        LOGI("toplevel-capture: window list for instance '", instance, "'");
        enclaves[instance] = list;
        return list;
    }

    // Which list global a client may see: its own instance's, or the personal
    // one; never another instance's, never a list this plugin does not own.
    bool global_visible(const wl_client *client, const wl_global *global)
    {
        if (auto owner = screen_owner(global))
        {
            auto ctx = sc::lookup_client(client);
            return ctx && (ctx->instance_id == *owner);
        }

        if (personal_screens && (global == personal_screens->global))
        {
            return !sc::lookup_client(client);
        }

        // Core's draws the screen as it is, enclave windows and all, so no
        // client sees it: an enclave gets its own, a person the veiled one.
        auto real = wf::get_core().protocols.output_image_capture_source;
        if (real && (global == real->global))
        {
            return false;
        }

        if (std::string(wl_global_get_interface(global)->name) !=
            wlr_ext_foreign_toplevel_list_v1_interface_name())
        {
            return true;
        }

        auto ctx = sc::lookup_client(client);
        if (personal && (global == personal->global))
        {
            return !ctx;
        }

        for (auto& [instance, list] : enclaves)
        {
            if (list && (global == list->global))
            {
                return ctx && (ctx->instance_id == instance);
            }
        }

        return !ctx;
    }

    static const char *wlr_ext_foreign_toplevel_list_v1_interface_name()
    {
        return "ext_foreign_toplevel_list_v1";
    }

    void track(wayfire_view view)
    {
        auto toplevel = wf::toplevel_cast(view);
        if (!toplevel || toplevel->has_data(RECORD_DATA))
        {
            return;
        }

        auto rec  = std::make_unique<toplevel_record_t>();
        rec->view = toplevel;
        rec->ctx  = sc::lookup_client(view->get_client());
        std::string title = view->get_title(), app_id = view->get_app_id();
        wlr_ext_foreign_toplevel_handle_v1_state state{title.c_str(),
            app_id.c_str()};
        rec->personal = wlr_ext_foreign_toplevel_handle_v1_create(personal,
            &state);
        rec->personal->data = rec.get();
        if (rec->ctx)
        {
            rec->enclave = wlr_ext_foreign_toplevel_handle_v1_create(
                enclave_list(rec->ctx->instance_id), &state);
            rec->enclave->data = rec.get();
        }

        // Published for IPC (usher U-W2): the instance identifier is the one
        // a sandboxed client sees, so both sides can name a window without
        // its title. Each handle has its own, so both are kept.
        auto ids = std::make_unique<wf::toplevel_identifier::identifiers_t>();
        ids->personal = rec->personal->identifier;
        if (rec->enclave)
        {
            ids->instance = rec->enclave->identifier;
        }

        toplevel->store_data(std::move(ids),
            wf::toplevel_identifier::DATA_NAME);
        toplevel->store_data(std::move(rec), RECORD_DATA);
    }

    void handle_request(void *data);

    void veil(wayfire_view view)
    {
        if (view->has_data(VEIL_DATA) || !sc::lookup_client(view->get_client()))
        {
            return;
        }

        auto data  = std::make_unique<veil_data_t>();
        data->node = std::make_shared<veil_node_t>(view);
        wf::scene::add_front(view->get_surface_root_node(), data->node);
        view->store_data(std::move(data), VEIL_DATA);
    }

    void unveil(wayfire_view view)
    {
        if (auto d = view->get_data<veil_data_t>(VEIL_DATA))
        {
            if (d->node->parent())
            {
                wf::scene::remove_child(d->node);
            }

            view->erase_data(VEIL_DATA);
        }
    }

    wf::signal::connection_t<sc::context_committed_signal> on_committed =
        [=] (sc::context_committed_signal *ev)
    {
        enclave_list(ev->info.instance_id);
        screen_manager(ev->info.instance_id);
    };
    wf::signal::connection_t<wf::view_mapped_signal> on_mapped =
        [=] (wf::view_mapped_signal *ev)
    {
        track(ev->view);
        veil(ev->view);
    };
    wf::signal::connection_t<wf::view_unmapped_signal> on_unmapped =
        [=] (wf::view_unmapped_signal *ev)
    {
        ev->view->erase_data(wf::toplevel_identifier::DATA_NAME);
        ev->view->erase_data(RECORD_DATA);
        unveil(ev->view);
    };
    wf::signal::connection_t<wf::view_title_changed_signal> on_title =
        [=] (wf::view_title_changed_signal *ev) { update(ev->view); };
    wf::signal::connection_t<wf::view_app_id_changed_signal> on_app_id =
        [=] (wf::view_app_id_changed_signal *ev) { update(ev->view); };

    void update(wayfire_view view)
    {
        if (auto rec = view->get_data<toplevel_record_t>(RECORD_DATA))
        {
            rec->update_state();
        }
    }

  public:
    void init() override
    {
        auto display = wf::get_core().display;
        personal = wlr_ext_foreign_toplevel_list_v1_create(display,
            LIST_VERSION);
        manager =
            wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(
                display, 1);
        on_new_request.set_callback([=] (void *data) { handle_request(data); });
        on_new_request.connect(&manager->events.new_request);
        // Before the filter exists, so it is never shown unfiltered: wayland
        // filters at each advertisement, and there is no client yet anyway.
        personal_screens = std::make_unique<screen_manager_t>();
        personal_screens->source_for = [=] (wf::output_t *out)
        {
            return screen_for("", out);
        };
        personal_screens->global = wl_global_create(display,
            &ext_output_image_capture_source_manager_v1_interface, 1,
            personal_screens.get(), screen_manager_bind);

        filter = wf::get_core().create_global_filter();
        filter->set_filter([=] (const wl_client *c, const wl_global *g)
        {
            return global_visible(c, g);
        });

        scope_data->resolvers.push_back([=] (const wl_global *global)
        {
            return screen_owner(global);
        });
        for (auto& info : registry->committed)
        {
            enclave_list(info.instance_id);
            screen_manager(info.instance_id);
        }

        wf::get_core().output_layout->connect(&on_output_removed);

        registry->connect(&on_committed);
        wf::get_core().connect(&on_mapped);
        wf::get_core().connect(&on_unmapped);
        wf::get_core().connect(&on_title);
        wf::get_core().connect(&on_app_id);
        for (auto& view : wf::get_core().get_all_views())
        {
            if (view->is_mapped())
            {
                track(view);
                veil(view);
            }
        }
    }

    // wlroots offers no way to destroy these globals before the display goes.
    bool is_unloadable() override
    {
        return false;
    }

    // At shutdown wayfire dlcloses this .so the moment fini returns (tackup's
    // enable_so_unloading), so nothing of ours may outlive it. Every mapped
    // view carries our record, identifiers and veil node, whose code is here;
    // left on the view, its later destruction ran unmapped code and crashed
    // every shutdown (measured: plugins `ipc ipc-rules toplevel-capture`, 2
    // of 2). Undo for every view what the unmap handler undoes for one.
    void fini() override
    {
        for (auto& view : wf::get_core().get_all_views())
        {
            view->erase_data(wf::toplevel_identifier::DATA_NAME);
            view->erase_data(RECORD_DATA);
            unveil(view);
        }
    }
};

void wayfire_toplevel_capture::handle_request(void *data)
{
    using request_t =
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request;
    auto req = (request_t*)data;
    auto rec = req->toplevel_handle ?
        (toplevel_record_t*)req->toplevel_handle->data : nullptr;
    auto requester = sc::lookup_client(req->client);
    bool allowed   = rec && rec->view->is_mapped() &&
        (!requester || (rec->ctx && (rec->ctx->instance_id ==
            requester->instance_id)));
    if (!allowed)
    {
        // wlroots keeps the new object id private, so refuse by accepting a
        // throwaway source and destroying it at once: the client's resource
        // goes inert and any session on it reports `stopped`, which is a
        // clean refusal rather than a protocol error.
        LOGW("toplevel-capture: refused a capture request",
            requester ? " from instance '" + requester->instance_id + "'" : "");
        wlr_ext_image_capture_source_v1 refused;
        wlr_ext_image_capture_source_v1_init(&refused, &refused_impl);
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
            req, &refused);
        wlr_ext_image_capture_source_v1_finish(&refused);
        return;
    }

    // A person asking for an enclave window gets it black: the personal list
    // names every window (usher identifies them by it), so it can be asked.
    auto& slot = (!requester && rec->ctx) ? rec->veiled : rec->source;
    if (!slot)
    {
        slot = std::make_unique<view_source_t>(rec->view,
            &slot == &rec->veiled);
    }

    wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(req,
        &slot->wrap.base);
}

DECLARE_WAYFIRE_PLUGIN(wayfire_toplevel_capture);
