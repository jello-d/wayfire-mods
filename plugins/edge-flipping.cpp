// edge-flipping.cpp
//
// Flip workspaces by pushing the pointer into a screen edge. Per-output plugin:
// one instance per output, each watching pointer motion on its own output.
//
// Multi-output behavior is derived, never configured. An edge only flips if it
// is EXTERIOR -- on the outer boundary of the whole output layout. An edge that
// abuts a neighbouring output is INTERIOR: flipping there is suppressed so the
// pointer simply crosses to the next monitor. For a horizontal row of three
// portrait panels this yields left+top+bottom on the leftmost, right+top+bottom
// on the rightmost, and top+bottom only on the middle -- with no output names
// anywhere; it falls out of the geometry and adapts to any arrangement.
//
// Flips affect only THIS output. Crossing an exterior edge flips the firing
// output's own workspace set; cross-output coordination (moving the other
// panels in lockstep) is owned by the separate wsync plugin, which mirrors any
// output's workspace change onto the rest. Edge-flipping deliberately does NOT
// coordinate itself: when it did, its per-output broadcast and wsync both
// applied the delta and the two compounded -- on a 3-wide wrapping grid three
// stacked -1 flips summed to a net zero (you saw the flip and landed back at
// the start). One coordinator only; that one is wsync.

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>

#include <wayfire/plugin.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/core.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/seat.hpp>

#include <wayfire/signal-definitions.hpp>
#include <wayfire/signal-provider.hpp>

// wlroots headers are C and carry no extern "C" guard of their own, so a bare
// C++ include mangles their symbols (wlr_seat_get_keyboard et al.) and the
// plugin then fails to load with an undefined symbol. Wrap them.
extern "C" {
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
}

using steady_clock_t = std::chrono::steady_clock;

static const char *b2s(bool v) { return v ? "1" : "0"; }

class edge_flipping_t : public wf::per_output_plugin_instance_t {
 public:
  // Trigger zones.
  wf::option_wrapper_t<int> edge_px{"edge-flipping/edge_px"};
  wf::option_wrapper_t<int> approach_zone_px{"edge-flipping/approach_zone_px"};

  // Timing.
  wf::option_wrapper_t<int> max_approach_ms{"edge-flipping/max_approach_ms"};

  // Behavior.
  wf::option_wrapper_t<bool> enable_move_flip{"edge-flipping/enable_move_flip"};
  wf::option_wrapper_t<bool> enable_pointer_flip{
      "edge-flipping/enable_pointer_flip"};
  wf::option_wrapper_t<bool> wrap{"edge-flipping/wrap"};

  // Modifier gating.
  wf::option_wrapper_t<bool> require_modifier{"edge-flipping/require_modifier"};
  wf::option_wrapper_t<std::string> modifier{"edge-flipping/modifier"};

  // Performance knobs. check_interval_ms <= 0 means auto from max_approach_ms.
  wf::option_wrapper_t<int> check_interval_ms{
      "edge-flipping/check_interval_ms"};
  wf::option_wrapper_t<bool> debug_log{"edge-flipping/debug_log"};

  void init() override {
    cached_mods = parse_required_mods();
    modifier.set_callback([this] { cached_mods = parse_required_mods(); });

    compute_approach_cap();
    approach_zone_px.set_callback([this] { compute_approach_cap(); });

    compute_check_interval();
    max_approach_ms.set_callback([this] { compute_check_interval(); });
    check_interval_ms.set_callback([this] { compute_check_interval(); });

    compute_exterior_edges();

    // Recompute exterior edges whenever the layout changes (hotplug, kanshi
    // rotation, geometry edit). No output names: purely geometric.
    wf::get_core().output_layout->connect(&on_layout_changed);

    // Always log init -- confirms binary version and effective config.
    LOGI("EDGEFLIP init: BUILD=", __DATE__, " ", __TIME__,
         " output=",             output->handle->name,
         " edge_px=",            (int)edge_px,
         " approach_zone_px=",   (int)approach_zone_px,
         " (effective=",         effective_ap, ")",
         " max_approach_ms=",    (int)max_approach_ms,
         " wrap=",               b2s((bool)wrap),
         " exterior=L",          b2s(ext_left), "R", b2s(ext_right),
         "T",                    b2s(ext_top), "B", b2s(ext_bottom),
         " enable_pointer_flip=", b2s((bool)enable_pointer_flip),
         " enable_move_flip=",   b2s((bool)enable_move_flip),
         " require_modifier=",   b2s((bool)require_modifier),
         " modifier='",          (std::string)modifier, "'",
         " check_interval_ms=",  (int)check_interval_ms,
         " (effective=",         effective_check_interval, ")");

    wf::get_core().connect(&on_motion_rel);
    wf::get_core().connect(&on_motion_abs);
  }

  void fini() override {
    on_motion_rel.disconnect();
    on_motion_abs.disconnect();
    on_layout_changed.disconnect();
  }

 private:
  // Per-edge approach state. Each edge is tracked independently so a disarm on
  // one side never blocks the opposite side.
  //
  // A flip requires a fresh approach: the cursor must fully leave the approach
  // zone (resetting state) then re-enter and reach the edge within
  // max_approach_ms. Lingering disarms. Firing disarms. The only way to re-arm
  // is to leave the approach zone entirely.
  struct approach_state_t {
    // in_zone: was the cursor in this approach zone on the previous event?
    // Snapshotted before update_approach_state runs so the entry event can
    // never fire -- entry_time is just being set and no traversal time has
    // been measured yet.
    bool in_zone = false;
    bool disarmed = false;
    steady_clock_t::time_point entry_time{};

    void reset() {
      in_zone = false;
      disarmed = false;
      entry_time = {};
    }
  };

  approach_state_t left_approach;
  approach_state_t right_approach;
  approach_state_t top_approach;
  approach_state_t bottom_approach;

  // Which of this output's edges are exterior (on the layout's outer boundary).
  // An interior edge abuts a neighbouring output; flipping there is suppressed
  // so the pointer crosses to that monitor. Recomputed on layout change.
  bool ext_left = true;
  bool ext_right = true;
  bool ext_top = true;
  bool ext_bottom = true;

  // Effective approach zone px -- approach_zone_px capped at 1/3 of the smaller
  // output dimension to prevent overlapping opposite edges.
  int effective_ap = 0;

  // Effective throttle interval in ms. When check_interval_ms <= 0,
  // auto-computed as max_approach_ms / 10, giving ~10 samples across the
  // approach window for accurate velocity measurement.
  int effective_check_interval = 0;

  // Cached modifier bitmask -- recomputed only on config change.
  uint32_t cached_mods = 0;

  // Throttle.
  steady_clock_t::time_point last_check{};

  // Determine which edges of this output sit on the outer boundary of the whole
  // output layout. An edge is INTERIOR if some other output is adjacent on that
  // side (its opposite edge lies on our edge line and the perpendicular spans
  // overlap). Otherwise the edge is EXTERIOR and eligible to flip. Pure
  // geometry -- no output names, works for any arrangement.
  void compute_exterior_edges() {
    const auto me = output->get_layout_geometry();

    const int my_l = me.x;
    const int my_r = me.x + me.width;
    const int my_t = me.y;
    const int my_b = me.y + me.height;

    bool nbr_left = false, nbr_right = false;
    bool nbr_top = false, nbr_bottom = false;

    for (auto wo : wf::get_core().output_layout->get_outputs()) {
      if (wo == output) continue;

      const auto g = wo->get_layout_geometry();
      if ((g.width <= 0) || (g.height <= 0)) continue;

      const int o_l = g.x;
      const int o_r = g.x + g.width;
      const int o_t = g.y;
      const int o_b = g.y + g.height;

      // Vertical overlap (for left/right adjacency).
      const bool v_overlap = (o_t < my_b) && (o_b > my_t);
      // Horizontal overlap (for top/bottom adjacency).
      const bool h_overlap = (o_l < my_r) && (o_r > my_l);

      // Neighbour touches our left edge: its right edge == our left, and the
      // vertical spans overlap. (== abutting; small tolerance not needed since
      // the layout packs outputs exactly.)
      if (v_overlap && (o_r == my_l)) nbr_left = true;
      if (v_overlap && (o_l == my_r)) nbr_right = true;
      if (h_overlap && (o_b == my_t)) nbr_top = true;
      if (h_overlap && (o_t == my_b)) nbr_bottom = true;
    }

    ext_left = !nbr_left;
    ext_right = !nbr_right;
    ext_top = !nbr_top;
    ext_bottom = !nbr_bottom;
  }

  wf::signal::connection_t<wf::output_layout_configuration_changed_signal>
      on_layout_changed =
          [=](wf::output_layout_configuration_changed_signal *) {
            compute_exterior_edges();
            reset_all_state();
          };

  void compute_approach_cap() {
    const auto og = output->get_relative_geometry();
    const int limit = std::min(og.width, og.height) / 3;
    const int requested = (int)approach_zone_px;

    if (requested > limit) {
      LOGW("EDGEFLIP: approach_zone_px=", requested, " exceeds 1/3 of output (",
           og.width, "x", og.height, "), capping at ", limit);
      effective_ap = limit;
    } else {
      effective_ap = requested;
    }
  }

  void compute_check_interval() {
    const int configured = (int)check_interval_ms;
    if (configured <= 0)
      effective_check_interval = std::max(1, (int)max_approach_ms / 10);
    else
      effective_check_interval = configured;
  }

  wf::signal::connection_t<
      wf::post_input_event_signal<wlr_pointer_motion_event>>
      on_motion_rel =
          [=](wf::post_input_event_signal<wlr_pointer_motion_event> *) {
            handle_motion();
          };

  wf::signal::connection_t<
      wf::post_input_event_signal<wlr_pointer_motion_absolute_event>>
      on_motion_abs =
          [=](wf::post_input_event_signal<
                  wlr_pointer_motion_absolute_event> *) {
            handle_motion();
          };

  static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
  }

  bool throttled(steady_clock_t::time_point now) {
    if (effective_check_interval <= 0) return false;

    const auto dt = std::chrono::milliseconds(effective_check_interval);
    if ((now - last_check) < dt) return true;

    last_check = now;
    return false;
  }

  uint32_t parse_required_mods() const {
    const std::string s = (std::string)modifier;
    uint32_t req = 0;

    auto has = [&](const char *tok) -> bool {
      return s.find(tok) != std::string::npos;
    };

    if (has("<shift>")) req |= WLR_MODIFIER_SHIFT;
    if (has("<ctrl>")) req |= WLR_MODIFIER_CTRL;
    if (has("<alt>")) req |= WLR_MODIFIER_ALT;
    if (has("<super>")) req |= WLR_MODIFIER_LOGO;

    return req;
  }

  uint32_t current_modifiers() const {
    wf::seat_t *seat = wf::get_core().seat.get();
    if (!seat || !seat->seat) return 0;

    wlr_keyboard *kbd = wlr_seat_get_keyboard(seat->seat);
    if (!kbd) return 0;

    return wlr_keyboard_get_modifiers(kbd);
  }

  bool modifiers_satisfied() const {
    if (!(bool)require_modifier) return true;
    if (cached_mods == 0) return false;
    return (current_modifiers() & cached_mods) == cached_mods;
  }

  bool move_grab_active() const {
    return output && output->is_plugin_active("move");
  }

  bool flipping_enabled() const {
    if (!modifiers_satisfied()) return false;
    if ((bool)enable_pointer_flip) return true;
    if ((bool)enable_move_flip && move_grab_active()) return true;
    return false;
  }

  void update_approach_state(approach_state_t& state, bool in_zone_now,
                             steady_clock_t::time_point now, int max_ms,
                             const char *name) {
    if (!in_zone_now) {
      state.reset();
      return;
    }

    if (!state.in_zone) {
      // Entry event: record time but do not allow firing yet. in_zone=true on
      // the next event means traversal has begun.
      state.in_zone = true;
      state.disarmed = false;
      state.entry_time = now;

      if ((bool)debug_log)
        LOGI("EDGEFLIP: entered ", name, " approach zone");

      return;
    }

    // Already in zone from a previous event -- check disarm timer.
    if (!state.disarmed && max_ms > 0) {
      using ms = std::chrono::milliseconds;
      const long elapsed =
          std::chrono::duration_cast<ms>(now - state.entry_time).count();

      if (elapsed >= max_ms) {
        state.disarmed = true;

        if ((bool)debug_log)
          LOGI("EDGEFLIP: ", name, " approach disarmed (lingered ", elapsed,
               " ms)");
      }
    }
  }

  void reset_all_state() {
    left_approach.reset();
    right_approach.reset();
    top_approach.reset();
    bottom_approach.reset();
  }

  void handle_motion() {
    const auto now = steady_clock_t::now();

    if (!flipping_enabled()) {
      reset_all_state();
      return;
    }

    if (throttled(now)) return;

    const auto cursor = wf::get_core().get_cursor_position();
    const wf::point_t ipos{(int)cursor.x, (int)cursor.y};
    // MUST be layout geometry (global coords), not relative ({0,0,w,h}).
    // get_cursor_position() returns GLOBAL layout coordinates. Relative
    // geometry pins every output's box at origin (0,0), so the global cursor
    // is tested against the wrong box -- correct only for the output sitting at
    // layout origin. That leaves far (non-origin) exterior edges unreachable
    // and lets every instance claim a cursor that is really on one output
    // (triple-firing on shared edges). Layout geometry puts cursor and bounds
    // in one space; this is the same call compute_exterior_edges() uses.
    const auto og = output->get_layout_geometry();

    // Cursor outside this output?
    if ((ipos.x < og.x) || (ipos.y < og.y) || (ipos.x >= og.x + og.width) ||
        (ipos.y >= og.y + og.height)) {
      reset_all_state();
      return;
    }

    const int lx = ipos.x - og.x;
    const int ly = ipos.y - og.y;

    const int ep = (int)edge_px;
    const int ap = effective_ap;
    const int max_ap_ms = (int)max_approach_ms;

    const bool approach_enabled = (ap > ep);

    // Per-edge approach zone detection. Interior edges are forced off here:
    // their approach never arms, so they never flip -- the pointer just crosses
    // to the neighbouring output.
    const bool in_left_approach =
        ext_left && approach_enabled && (lx <= ap);
    const bool in_right_approach =
        ext_right && approach_enabled && (lx >= og.width - 1 - ap);
    const bool in_top_approach =
        ext_top && approach_enabled && (ly <= ap);
    const bool in_bottom_approach =
        ext_bottom && approach_enabled && (ly >= og.height - 1 - ap);

    // Per-edge zone detection, also gated by exterior-ness.
    const bool at_left = ext_left && (lx <= ep);
    const bool at_right = ext_right && (lx >= og.width - 1 - ep);
    const bool at_top = ext_top && (ly <= ep);
    const bool at_bottom = ext_bottom && (ly >= og.height - 1 - ep);
    const bool now_at_edge = at_left || at_right || at_top || at_bottom;

    if ((bool)debug_log) {
      LOGI("EDGEFLIP: pos=(", lx, ",", ly, ")",
           " edge=L", b2s(at_left), "R", b2s(at_right), "T", b2s(at_top), "B",
           b2s(at_bottom), " approach=L", b2s(in_left_approach), "R",
           b2s(in_right_approach), "T", b2s(in_top_approach), "B",
           b2s(in_bottom_approach));
    }

    // Snapshot in_zone BEFORE updating approach state. This is critical:
    // update_approach_state sets in_zone=true on the entry event, so reading
    // in_zone after the update would make the entry-event protection
    // completely ineffective -- the cursor could jump from outside the approach
    // zone to the edge in a single event and fire instantly. Using the
    // pre-update snapshot ensures at least one full check interval of traversal
    // time is measured before firing.
    const bool left_was_in_zone = left_approach.in_zone;
    const bool right_was_in_zone = right_approach.in_zone;
    const bool top_was_in_zone = top_approach.in_zone;
    const bool bottom_was_in_zone = bottom_approach.in_zone;

    // Update all approach states unconditionally -- entry_time and disarm state
    // must stay accurate whether or not we're at the edge.
    update_approach_state(left_approach, in_left_approach, now, max_ap_ms,
                          "left");
    update_approach_state(right_approach, in_right_approach, now, max_ap_ms,
                          "right");
    update_approach_state(top_approach, in_top_approach, now, max_ap_ms, "top");
    update_approach_state(bottom_approach, in_bottom_approach, now, max_ap_ms,
                          "bottom");

    if (!now_at_edge) return;

    // Each edge is gated by its own approach state so a disarm on one side
    // never blocks the opposite side. was_in_zone uses the pre-update snapshot
    // to ensure the cursor was already in the approach zone on the previous
    // event -- preventing firing on the entry event before any traversal time
    // has been measured.
    const bool left_allowed =
        !approach_enabled || (left_was_in_zone && !left_approach.disarmed);
    const bool right_allowed =
        !approach_enabled || (right_was_in_zone && !right_approach.disarmed);
    const bool top_allowed =
        !approach_enabled || (top_was_in_zone && !top_approach.disarmed);
    const bool bot_allowed =
        !approach_enabled || (bottom_was_in_zone && !bottom_approach.disarmed);

    int target_dx = 0;
    int target_dy = 0;

    if (at_left && left_allowed) target_dx = -1;
    else if (at_right && right_allowed) target_dx = 1;

    if (at_top && top_allowed) target_dy = -1;
    else if (at_bottom && bot_allowed) target_dy = 1;

    if (target_dx == 0 && target_dy == 0) {
      if ((bool)debug_log)
        LOGI("EDGEFLIP: at edge but disarmed or not yet in zone");
      return;
    }

    // Pre-check: if the grid boundary would block this flip, disarm the
    // relevant edge so we don't re-evaluate on every event while the cursor
    // sits at a grid boundary. Uses this output's own grid; with a uniform
    // grid across outputs the boundary is the same everywhere.
    {
      auto wset = output->wset();
      if (wset) {
        const auto cur = wset->get_current_workspace();
        const auto grid = wset->get_workspace_grid_size();

        const int nx = (bool)wrap
            ? ((cur.x + target_dx + grid.width) % grid.width)
            : clampi(cur.x + target_dx, 0, grid.width - 1);
        const int ny = (bool)wrap
            ? ((cur.y + target_dy + grid.height) % grid.height)
            : clampi(cur.y + target_dy, 0, grid.height - 1);

        if (nx == cur.x && ny == cur.y) {
          if ((bool)debug_log)
            LOGI("EDGEFLIP: boundary blocks flip dx=", target_dx,
                 " dy=", target_dy, " at workspace ", cur.x, ",", cur.y);

          if (target_dx == -1) left_approach.disarmed = true;
          if (target_dx == 1) right_approach.disarmed = true;
          if (target_dy == -1) top_approach.disarmed = true;
          if (target_dy == 1) bottom_approach.disarmed = true;
          return;
        }
      }
    }

    if ((bool)debug_log)
      LOGI("EDGEFLIP: firing dx=", target_dx, " dy=", target_dy, " at (", lx,
           ",", ly, ")");

    // Flip THIS output only. wsync mirrors the resulting workspace change onto
    // the other outputs, so the array still moves in lockstep -- but the delta
    // is applied exactly once, here, instead of once per output.
    apply_flip(target_dx, target_dy);

    // Disarm all edges on THIS instance after firing. The cursor is still
    // physically inside our approach zone -- without this the next motion event
    // would fire again. Disarming forces a full withdraw-and-re-approach. Only
    // leaving the approach zone (triggering reset()) re-arms them.
    left_approach.disarmed = true;
    right_approach.disarmed = true;
    top_approach.disarmed = true;
    bottom_approach.disarmed = true;
  }

  // Apply a workspace delta to THIS output's wset. Honors wrap/clamp. Called
  // once from the firing path on the output whose edge was crossed; wsync then
  // mirrors the resulting change to the other outputs.
  void apply_flip(int dx, int dy) {
    auto wset = output->wset();
    if (!wset) {
      LOGE("EDGEFLIP: no workspace set");
      return;
    }

    const auto cur = wset->get_current_workspace();
    const auto grid = wset->get_workspace_grid_size();

    int nx = cur.x + dx;
    int ny = cur.y + dy;

    const int maxx = grid.width - 1;
    const int maxy = grid.height - 1;

    if ((bool)wrap) {
      if (nx < 0) nx = maxx;
      if (nx > maxx) nx = 0;
      if (ny < 0) ny = maxy;
      if (ny > maxy) ny = 0;
    } else {
      nx = clampi(nx, 0, maxx);
      ny = clampi(ny, 0, maxy);
    }

    if (nx == cur.x && ny == cur.y) {
      if ((bool)debug_log)
        LOGI("EDGEFLIP: flip blocked (already at ", cur.x, ",", cur.y, ")");
      return;
    }

    if ((bool)debug_log)
      LOGI("EDGEFLIP: flip ", cur.x, ",", cur.y, " -> ", nx, ",", ny,
           " (delta ", dx, ",", dy, " grid ", grid.width, "x", grid.height,
           " wrap=", b2s((bool)wrap), " on ", output->handle->name, ")");

    wset->request_workspace({nx, ny});
  }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<edge_flipping_t>);
