// edge-warp.cpp
//
// Carry the pointer across a JAGGED edge. Where two outputs of different
// sizes sit side by side, part of the edge between them has nothing beyond
// it, and wlroots clamps the pointer to the layout there: pushing across that
// part stops it dead, a wall in the middle of the desk. This plugin carries
// it on to the neighbouring output instead, straight across and slid along
// the edge to the nearest point, as if the edges lined up. Optionally
// PROPORTIONAL, mapping the whole edge onto the whole neighbouring edge.
// The geometry, and exactly when it acts, is edge-warp-geom.hpp.
//
// INDEPENDENT OF edge-flipping BY CONSTRUCTION, not by coordination: this
// acts only on an edge some output touches, and edge-flipping flips only on
// an edge NO output touches (an edge with any neighbour is wholly interior to
// it). So the two never act on the same push, each can be on or off alone,
// and neither knows the other exists. test/edge-warp.t holds edge-flipping
// to that rule, since it is the one thing this independence rests on.
//
// HOW: it rewrites the motion's delta before core applies it, the way core's
// own pointer-constraint handling and the cube plugin do, so the cursor lands
// where the rewritten delta points and nothing else moves it twice.
//
// IT STANDS ASIDE when a hand on a mouse is not simply moving the pointer:
//   - a button is held: a window move or resize, a drag, a text selection.
//     Carrying those across would throw the window or the selection. Counted
//     HERE, from the raw device events, not from the seat: wlroots' seat
//     counts only buttons it delivered to a CLIENT, so a press on the
//     background or into a compositor grab read as no button at all
//     (measured: a held button still warped).
//   - the surface under the pointer has a pointer constraint (a game locking
//     or confining it). Core re-confines the delta in that case anyway; not
//     touching it also keeps relative-pointer clients' deltas the raw ones.
// Absolute motion (tablets, touch) is never seen: only relative motion is.
//
// `enabled` is an OPTION rather than the plugin's presence, so a desk can
// switch it live over IPC: changing the plugin list in a live session is what
// crashed one (hwdp H7).

#include <set>
#include <vector>

#include <wayfire/core.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/signal-provider.hpp>

// wlroots headers are C with no extern "C" guard of their own (see
// edge-flipping.cpp).
extern "C" {
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_seat.h>
}

#include "edge-warp-geom.hpp"

class edge_warp_t : public wf::plugin_interface_t {
  wf::option_wrapper_t<bool> enabled{"edge-warp/enabled"};
  wf::option_wrapper_t<bool> proportional{"edge-warp/proportional"};
  wf::option_wrapper_t<int> inset_px{"edge-warp/inset_px"};

 public:
  void init() override {
    wf::get_core().connect(&on_button);
    wf::get_core().connect(&on_motion);
  }
  void fini() override {
    on_motion.disconnect();
    on_button.disconnect();
  }

 private:
  // Read from the live layout at the moment of the push, never cached, so a
  // hotplug or a kanshi re-layout cannot leave it describing an old desk.
  static std::vector<ew_rect> layout_rects() {
    std::vector<ew_rect> r;
    for (auto *wo : wf::get_core().output_layout->get_outputs()) {
      const auto g = wo->get_layout_geometry();
      if (g.width > 0 && g.height > 0)
        r.push_back({double(g.x), double(g.y), double(g.width),
                     double(g.height)});
    }
    return r;
  }

  // Buttons down now, by code. A release with no press (the plugin loaded
  // while one was held) is simply nothing to erase.
  std::set<uint32_t> held;

  wf::signal::connection_t<wf::input_event_signal<wlr_pointer_button_event>>
      on_button = [=](wf::input_event_signal<wlr_pointer_button_event> *sig) {
        if (sig->event->state == WL_POINTER_BUTTON_STATE_PRESSED)
          held.insert(sig->event->button);
        else
          held.erase(sig->event->button);
      };

  bool hands_busy() const {
    if (!held.empty()) return true;
    wlr_seat *seat = wf::get_core().get_current_seat();
    if (!seat) return true;
    wlr_surface *s = seat->pointer_state.focused_surface;
    auto *pc = wf::get_core().protocols.pointer_constraints;
    return s && pc &&
           wlr_pointer_constraints_v1_constraint_for_surface(pc, s, seat);
  }

  wf::signal::connection_t<wf::input_event_signal<wlr_pointer_motion_event>>
      on_motion = [=](wf::input_event_signal<wlr_pointer_motion_event> *sig) {
        if (!enabled || hands_busy()) return;
        auto *ev = sig->event;
        const auto c = wf::get_core().get_cursor_position();
        ew_point to;
        if (!edge_warp::target(layout_rects(), {c.x, c.y}, ev->delta_x,
                               ev->delta_y, proportional,
                               double(int(inset_px)), to))
          return;
        ev->delta_x = to.x - c.x;
        ev->delta_y = to.y - c.y;
      };
};

DECLARE_WAYFIRE_PLUGIN(edge_warp_t);
