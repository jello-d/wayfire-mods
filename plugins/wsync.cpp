// wsync.cpp
//
// Keep every output on the same workspace. Per-output plugin: one instance per
// output, each watching its own output's workspace_changed signal.
//
// With independent workspace sets (Wayfire's default; no [wsets] config) each
// output flips in isolation. wsync makes the array move as one: when ANY output
// changes workspace, by any mechanism, edge-flip, expo selection, vswitch,
// grid, or a raw IPC request_workspace, every other output's wset is driven
// to the same viewport. Coordination lives here, at the workspace-change layer,
// so no trigger has to know about it: the invariant is enforced in one place.
//
// REENTRANCY. Applying the mirror calls request_workspace() on the other wsets,
// which itself emits workspace_changed on those outputs and re-enters this
// handler. Two independent guards make that safe:
//   1. g_syncing: a plugin-wide flag (all instances share it; same .so)
//      bracketing the whole mirror pass. Re-entrant emissions land while it is
//      set and bail immediately. This is the primary guard and assumes the
//      emission is synchronous within request_workspace().
//   2. the "already at target" short-circuit: even if an emission is instead
//      delivered deferred (after g_syncing clears), the wset state was already
//      moved synchronously by request_workspace(), so every output reads
//      current == target and the pass changes nothing and emits nothing
//      further. Convergence holds whether emission is sync or async.
//
// ESCAPE HATCH. wsync/enabled (default true) gates the whole thing. Set it
// false to let outputs hold different workspaces for a presentation, then true
// to re-attach. It live-reloads, so no restart. There is deliberately no
// keybind in the plugin; bind one externally via the wayfire/set-config-options
// IPC method if you want a hotkey.

#include <wayfire/plugin.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/core.hpp>
#include <wayfire/workspace-set.hpp>

#include <wayfire/signal-definitions.hpp>
#include <wayfire/signal-provider.hpp>

static const char *b2s(bool v) { return v ? "1" : "0"; }

// Plugin-wide reentrancy guard, shared by every per-output instance because
// they live in the same .so. True while a mirror pass is applying workspace
// changes to the other outputs, so the workspace_changed emissions those
// changes trigger are recognised as our own and ignored.
static bool g_syncing = false;

class wsync_t : public wf::per_output_plugin_instance_t {
 public:
  wf::option_wrapper_t<bool> enabled{"wsync/enabled"};
  wf::option_wrapper_t<bool> debug_log{"wsync/debug_log"};

  void init() override {
    output->connect(&on_ws_changed);
    adopt();

    LOGI("WSYNC init: BUILD=", __DATE__, " ", __TIME__,
         " output=",  output->handle->name,
         " enabled=", b2s((bool)enabled));
  }

  void fini() override {
    on_ws_changed.disconnect();
  }

 private:
  static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }

  // Drive every output's wset to `target`. Bracketed by g_syncing so the
  // request_workspace() calls below do not ricochet back through this handler.
  // Each output's target is clamped to its own grid; today all grids match
  // (3x3) so the clamp is inert, but keeps a heterogeneous future arrangement
  // from requesting an out-of-range viewport.
  void sync_to(wf::point_t target) {
    g_syncing = true;

    for (auto wo : wf::get_core().output_layout->get_outputs()) {
      auto wset = wo->wset();
      if (!wset) continue;

      const auto grid = wset->get_workspace_grid_size();
      const wf::point_t t{clampi(target.x, 0, grid.width - 1),
                          clampi(target.y, 0, grid.height - 1)};

      const auto cur = wset->get_current_workspace();
      if (cur.x == t.x && cur.y == t.y) continue;

      if ((bool)debug_log)
        LOGI("WSYNC: ", wo->handle->name, " ", cur.x, ",", cur.y, " -> ",
             t.x, ",", t.y);

      wset->request_workspace(t);
    }

    g_syncing = false;
  }

  // A NEW output joins the array where it already is. Wayfire gives an output
  // that arrives (a monitor plugged in or woken, one switched back on) a
  // fresh wset at (0,0), and no workspace_changed fires for that, so without
  // this it sits on (0,0) while the rest stay put, until the next switch.
  // The first OTHER output is the reference: they are all in sync already.
  void adopt() {
    if (!(bool)enabled) return;
    auto mine = output->wset();
    if (!mine) return;
    for (auto wo : wf::get_core().output_layout->get_outputs()) {
      if (wo == output || !wo->wset()) continue;
      const auto grid = mine->get_workspace_grid_size();
      const auto ref = wo->wset()->get_current_workspace();
      const wf::point_t t{clampi(ref.x, 0, grid.width - 1),
                          clampi(ref.y, 0, grid.height - 1)};
      const auto cur = mine->get_current_workspace();
      if (cur.x == t.x && cur.y == t.y) return;
      if ((bool)debug_log)
        LOGI("WSYNC: ", output->handle->name, " joins at ", t.x, ",", t.y);
      g_syncing = true;
      mine->request_workspace(t);
      g_syncing = false;
      return;
    }
  }

  // A workspace changed on THIS output. Mirror it to the rest, unless sync is
  // disabled or we are the ones mid-applying it.
  wf::signal::connection_t<wf::workspace_changed_signal> on_ws_changed =
      [=](wf::workspace_changed_signal *ev) {
        if (!(bool)enabled) return;
        if (g_syncing) return;
        sync_to(ev->new_viewport);
      };
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wsync_t>);
