// idle-audit.cpp
//
// WHO IS HOLDING IDLE OFF. Wayfire keeps idle inhibition as one private
// counter (src/core/idle.cpp): every client inhibitor bumps it, its
// destruction drops it, and while it is non-zero the idle notifier is told to
// deliver nothing to any client that obeys inhibitors. So when an idle timer
// goes deaf, the compositor can say THAT idle is inhibited but nothing can say
// WHO, and a leaked inhibitor, a legitimate call and a wayfire bug all look
// identical from outside. Measured twice in this fleet: a box whose idle timer
// heard nothing for hours, a fresh subscription equally deaf, and no way to
// name the holder.
//
// wlroots keeps the list wayfire does not: the inhibit manager's `inhibitors`
// is every live client inhibitor, each with its surface and its client. So
// this plugin needs no core patch. It answers one IPC method,
//
//   idle-audit/list  ->  { "inhibited": bool,
//                          "consistent": bool,
//                          "inhibitors": [ { "pid", "uid", "comm",
//                              "since", "view-id", "app-id", "title",
//                              "mapped", "minimized", "visible" }, ... ] }
//
// and logs every inhibitor's creation and destruction with the same identity,
// so the NEXT deaf idle timer leaves a trail of who took it and when.
//
// `inhibited` is the notifier's own flag, read live: it is exactly what
// decides whether an idle client hears anything, so it is the fact worth
// reporting rather than an inference from the list. `consistent` is false when
// that flag disagrees with the list (inhibited with no live inhibitor, or the
// reverse). No plugin in this fleet takes an inhibitor of its own (stock's
// `idle` plugin can, and is not loaded here), so a disagreement is a wayfire
// counter that has drifted from reality: a bug, named as one.
//
// IT REVOKES NOTHING, deliberately. A live client's inhibitor is often exactly
// the one that must hold (an enclave call running behind other work), so
// deciding which to override is policy, and it belongs to whoever reads this,
// with the evidence in hand. Wayfire also counts an inhibitor whether or not
// its window is visible, which the protocol permits; `visible` is reported so
// that question can be asked rather than assumed.
//
// AN ENCLAVE CLIENT'S pid IS ITS OWN. Measured on dragonfly (2026-11-16,
// tackup TW1): titan's Chrome reached wayfire directly through its
// security-context socket, so wayfire saw Chrome's own pid under the enclave's
// uid. An earlier version of this comment said the pid was a relay's
// (severance-display); that was never measured. Were a relay ever in the
// path, the pid here would be the relay's, and window-rules/view-info on
// `view-id` carries the security context either way.
//
// IT ALSO MARKS THE HOLDER, for the eye rather than for IPC: each toplevel
// holding an inhibitor carries a count (idle-holder.hpp) and a signal goes
// out on every change, which enclave-mark turns into a halo on the window.
// An inhibitor taken on a surface before its window maps is counted when the
// window maps.

#include <wayfire/plugin.hpp>
#include <wayfire/core.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/output.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/util.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/plugins/common/shared-core-data.hpp>
#include <wayfire/plugins/ipc/ipc-method-repository.hpp>
#include "idle-holder.hpp"

#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
struct identity_t
{
  pid_t pid = 0;
  uid_t uid = 0;
  std::string comm;
};

identity_t identify(wlr_idle_inhibitor_v1 *inh)
{
  identity_t id;
  gid_t gid;
  wl_client *client = wl_resource_get_client(inh->resource);
  wl_client_get_credentials(client, &id.pid, &id.uid, &gid);
  std::ifstream f("/proc/" + std::to_string(id.pid) + "/comm");
  std::getline(f, id.comm);
  return id;
}

// The toplevel a (possibly sub-)surface belongs to, or null: a layer-shell
// surface, an unmapped one, or one with no wayfire view at all.
wayfire_toplevel_view view_of(wlr_idle_inhibitor_v1 *inh)
{
  if (!inh->surface)
  {
    return nullptr;
  }

  wlr_surface *root = wlr_surface_get_root_surface(inh->surface);
  return wf::toplevel_cast(wf::wl_surface_to_wayfire_view(root->resource));
}

// On screen as a human would mean it: mapped, not minimized, in a workspace
// set an output is showing, on that set's current workspace.
bool is_visible(wayfire_toplevel_view v)
{
  if (!v->is_mapped() || v->minimized)
  {
    return false;
  }

  auto wset = v->get_wset();
  if (!wset || !wset->get_attached_output())
  {
    return false;
  }

  return wset->get_view_main_workspace(v) == wset->get_current_workspace();
}
}

class idle_audit_t : public wf::plugin_interface_t
{
  wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> ipc;
  wf::wl_listener_wrapper on_new;

  struct tracked_t
  {
    time_t since;
    identity_t id;
    std::unique_ptr<wf::wl_listener_wrapper> on_destroy;
    // The window this inhibitor is counted on, once it has one.
    std::weak_ptr<wf::view_interface_t> counted;
  };
  std::map<wlr_idle_inhibitor_v1*, tracked_t> tracked;
  // A listener cannot be freed from inside its own callback, so a released
  // inhibitor's listener waits here until the next call into the plugin.
  std::vector<std::unique_ptr<wf::wl_listener_wrapper>> graveyard;

  static void announce(wayfire_view v)
  {
    wf::idle_holder::changed_signal ev;
    ev.view = v;
    wf::get_core().emit(&ev);
  }

  // Count `inh` on its window, if it has one and is not counted yet.
  static void count_on(tracked_t& t, wlr_idle_inhibitor_v1 *inh)
  {
    if (!t.counted.expired())
    {
      return;
    }

    auto v = view_of(inh);
    if (!v)
    {
      return;
    }

    auto d = v->get_data_safe<wf::idle_holder::holder_t>(
      wf::idle_holder::DATA_NAME);
    t.counted = v->shared_from_this();
    if (++d->count == 1)
    {
      announce(v);
    }
  }

  static void uncount(tracked_t& t)
  {
    auto v = t.counted.lock();
    t.counted.reset();
    auto d = v ? v->get_data<wf::idle_holder::holder_t>(
      wf::idle_holder::DATA_NAME) : nullptr;
    if (d && (--d->count <= 0))
    {
      v->erase_data(wf::idle_holder::DATA_NAME);
      announce(wayfire_view{v.get()});
    }
  }

  // A surface can take an inhibitor before its window maps.
  wf::signal::connection_t<wf::view_mapped_signal> on_mapped =
    [this] (wf::view_mapped_signal*)
  {
    for (auto& [inh, t] : tracked)
    {
      count_on(t, inh);
    }
  };

  void track(wlr_idle_inhibitor_v1 *inh, time_t since)
  {
    graveyard.clear();
    auto& t = tracked[inh];
    t.since = since;
    t.id = identify(inh);
    t.on_destroy = std::make_unique<wf::wl_listener_wrapper>();
    t.on_destroy->set_callback([this, inh] (void*)
    {
      auto it = tracked.find(inh);
      if (it == tracked.end())
      {
        return;
      }

      LOGI("idle-audit: inhibitor released by pid ", it->second.id.pid,
        " (", it->second.id.comm, ")", it->second.since ?
        ", held " + std::to_string(time(nullptr) - it->second.since) + "s" :
        std::string(", taken before idle-audit loaded"));
      uncount(it->second);
      it->second.on_destroy->disconnect();
      graveyard.push_back(std::move(it->second.on_destroy));
      tracked.erase(it);
    });
    t.on_destroy->connect(&inh->events.destroy);
    count_on(t, inh);
  }

  wf::json_t describe(wlr_idle_inhibitor_v1 *inh)
  {
    auto it = tracked.find(inh);
    auto id = (it != tracked.end()) ? it->second.id : identify(inh);
    wf::json_t j;
    j["pid"]  = (int64_t)id.pid;
    j["uid"]  = (int64_t)id.uid;
    j["comm"] = id.comm;
    // Integers are ASSIGNED through a reference, never passed to json_t's
    // constructor: json_t(int64_t) is `*this = v`, and json_t's own
    // operator= hides the base's int64_t one, so the constructor calls
    // itself until the stack runs out. Measured: the compositor died with
    // ~40000 frames of json.cpp:485 the first time a `since` was serialised.
    if (it != tracked.end() && it->second.since)
    {
      j["since"] = (int64_t)it->second.since;
    } else
    {
      j["since"] = wf::json_t::null();
    }

    auto v = view_of(inh);
    if (v)
    {
      j["view-id"] = (int64_t)v->get_id();
    } else
    {
      j["view-id"] = wf::json_t::null();
    }

    j["app-id"]    = v ? v->get_app_id() : std::string{};
    j["title"]     = v ? v->get_title() : std::string{};
    j["mapped"]    = v ? v->is_mapped() : false;
    j["minimized"] = v ? v->minimized : false;
    j["visible"]   = v ? is_visible(v) : false;
    return j;
  }

  wf::ipc::method_callback list = [this] (wf::json_t)
  {
    graveyard.clear();
    auto *mgr = wf::get_core().protocols.idle_inhibit;
    // The notifier's flag is in wlroots' private section, which a consumer
    // build names WLR_PRIVATE. Read on purpose: it is the one fact that
    // decides whether an idle client hears anything. A wlroots bump that
    // moves it fails this build loudly rather than answering wrong.
    bool inhibited =
      wf::get_core().protocols.idle_notifier->WLR_PRIVATE.inhibited;
    wf::json_t all = wf::json_t::array();
    wlr_idle_inhibitor_v1 *inh;
    wl_list_for_each(inh, &mgr->inhibitors, link)
    {
      all.append(describe(inh));
    }

    wf::json_t r;
    r["inhibited"]  = inhibited;
    r["consistent"] = inhibited == (all.size() > 0);
    r["inhibitors"] = all;
    return r;
  };

 public:
  void init() override
  {
    auto *mgr = wf::get_core().protocols.idle_inhibit;
    // Already live when this loaded: tracked, with no creation time to give.
    wlr_idle_inhibitor_v1 *inh;
    wl_list_for_each(inh, &mgr->inhibitors, link)
    {
      track(inh, 0);
    }

    on_new.set_callback([this] (void *data)
    {
      auto *inh = static_cast<wlr_idle_inhibitor_v1*>(data);
      track(inh, time(nullptr));
      auto& id = tracked[inh].id;
      auto v   = view_of(inh);
      LOGI("idle-audit: inhibitor taken by pid ", id.pid, " (", id.comm,
        ") uid ", id.uid, " app-id '", v ? v->get_app_id() : "", "'");
    });
    on_new.connect(&mgr->events.new_inhibitor);
    wf::get_core().connect(&on_mapped);
    ipc->register_method("idle-audit/list", list);
  }

  void fini() override
  {
    ipc->unregister_method("idle-audit/list");
    on_new.disconnect();
    // Unmark every holder: the marks are only true while this plugin counts.
    for (auto& [inh, t] : tracked)
    {
      uncount(t);
      t.on_destroy->disconnect();
    }

    tracked.clear();
  }
};

DECLARE_WAYFIRE_PLUGIN(idle_audit_t);
