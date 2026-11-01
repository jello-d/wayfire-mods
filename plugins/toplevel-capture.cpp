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
// hands that to the copy-capture session. Whole-output capture is a separate
// global that the security-context allowlist does not offer to sandboxes.
//
// Replaces upstream `ext-toplevel`; do not load both.

#include <wayfire/core.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/util.hpp>
#include <wayfire/view.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/plugins/common/security-context.hpp>
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
}

namespace sc = wf::security_context;

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

    explicit view_source_t(wayfire_toplevel_view view);
    ~view_source_t();

    // Render instances kept alive for their DAMAGE callback: a window keeps
    // producing frames while it is on another workspace, or covered.
    void regen()
    {
        instances.clear();
        view->get_surface_root_node()->gen_render_instances(instances,
            [=] (const wf::regionf_t&) { mark_dirty(); }, view->get_output());
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

view_source_t::view_source_t(wayfire_toplevel_view view) : view(view)
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
    params.background_color = {0, 0, 0, 0};
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

// Per window: its handles in the lists it belongs to, and its source.
struct toplevel_record_t : public wf::custom_data_t
{
    wayfire_toplevel_view view;
    std::optional<sc::context_info_t> ctx;
    wlr_ext_foreign_toplevel_handle_v1 *personal = nullptr;
    wlr_ext_foreign_toplevel_handle_v1 *enclave  = nullptr;
    std::unique_ptr<view_source_t> source;

    ~toplevel_record_t()
    {
        source.reset();
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
    wf::wl_listener_wrapper on_new_request;

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

        toplevel->store_data(std::move(rec), RECORD_DATA);
    }

    void handle_request(void *data);

    wf::signal::connection_t<sc::context_committed_signal> on_committed =
        [=] (sc::context_committed_signal *ev)
    {
        enclave_list(ev->info.instance_id);
    };
    wf::signal::connection_t<wf::view_mapped_signal> on_mapped =
        [=] (wf::view_mapped_signal *ev) { track(ev->view); };
    wf::signal::connection_t<wf::view_unmapped_signal> on_unmapped =
        [=] (wf::view_unmapped_signal *ev)
    {
        ev->view->erase_data(RECORD_DATA);
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

        filter = wf::get_core().create_global_filter();
        filter->set_filter([=] (const wl_client *c, const wl_global *g)
        {
            return global_visible(c, g);
        });

        for (auto& info : registry->committed)
        {
            enclave_list(info.instance_id);
        }

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
            }
        }
    }

    // wlroots offers no way to destroy these globals before the display goes.
    bool is_unloadable() override
    {
        return false;
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

    if (!rec->source)
    {
        rec->source = std::make_unique<view_source_t>(rec->view);
    }

    wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(req,
        &rec->source->wrap.base);
}

DECLARE_WAYFIRE_PLUGIN(wayfire_toplevel_capture);
