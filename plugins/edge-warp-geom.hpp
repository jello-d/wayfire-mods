// edge-warp-geom.hpp
//
// The geometry of edge-warp, with no wayfire in it, so it can be tested on
// its own (test/edge-warp.t compiles it against a table of cases).
//
// One question: the pointer at C moves by (dx, dy); should it land somewhere
// else than where the layout's clamping would leave it? Only when it is
// pushed across an edge of the output it is on (C's output, O) that is
// PARTLY SHARED: some output N touches that side of O, but not at the point
// the pointer is crossing. Then it continues onto N, straight across and
// slid along the edge to the nearest point of N, as if the edges lined up.
// An edge no output touches anywhere is left alone: that is the layout's
// outer boundary, a wall or edge-flipping's to act on, never this plugin's.
//
// PROPORTIONAL instead maps the whole of O's edge onto the whole of N's, so
// the same fraction of the way along lands at the same fraction on N. That
// changes the shared part too, which is why it is not the default. It needs
// exactly ONE output on that side (with two, "the whole edge" is two
// different maps), and falls back to the straight warp otherwise.
//
// INSET: the landing along the edge is kept INSET px inside the neighbour's
// span (never more than half of it). The nearest point is the neighbour's
// last row, which is its OUTER edge whenever the edges do not line up, and
// that row is where edge-flipping triggers, hot corners sit and a bottom bar
// auto-hides: measured, a warp landing there tripped an edge flip on the
// neighbour although the push was sideways. It applies to every landing,
// proportional ones included: mapping one panel's last row onto another's
// lands on that same outer edge.
//
// Rects are in layout coordinates, half-open: [x, x + w) by [y, y + h).

#pragma once

#include <cstddef>
#include <vector>

struct ew_rect {
  double x, y, w, h;
  double r() const { return x + w; }
  double b() const { return y + h; }
  bool contains(double px, double py) const {
    return px >= x && px < r() && py >= y && py < b();
  }
};

struct ew_point {
  double x, y;
};

namespace edge_warp {

inline double clampd(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// The side of O the target T leaves by, or 0 when it leaves by no single
// side (still inside, or past a corner, where the layout's own clamping is
// the right answer).
enum side_t { NONE = 0, LEFT, RIGHT, TOP, BOTTOM };

inline side_t side_left(const ew_rect &o, ew_point t) {
  const bool in_x = t.x >= o.x && t.x < o.r();
  const bool in_y = t.y >= o.y && t.y < o.b();
  if (in_y && t.x >= o.r()) return RIGHT;
  if (in_y && t.x < o.x) return LEFT;
  if (in_x && t.y >= o.b()) return BOTTOM;
  if (in_x && t.y < o.y) return TOP;
  return NONE;
}

// Does N touch O along side S (abutting, with the spans overlapping)?
inline bool touches(const ew_rect &o, const ew_rect &n, side_t s) {
  const bool v = n.y < o.b() && n.b() > o.y;
  const bool h = n.x < o.r() && n.r() > o.x;
  switch (s) {
    case RIGHT: return v && n.x == o.r();
    case LEFT: return v && n.r() == o.x;
    case BOTTOM: return h && n.y == o.b();
    case TOP: return h && n.b() == o.y;
    default: return false;
  }
}

// `along` clamped to [lo, hi), INSET inside it (at most half the span).
inline double inset_clamp(double along, double lo, double hi, double inset) {
  const double half = (hi - 1 - lo) / 2;
  const double in = inset < half ? inset : half;
  return clampd(along, lo + in, hi - 1 - in);
}

// Where T lands on N, crossing side S: the overshoot past O's edge carried
// onto N (kept inside it), and the along-edge coordinate `along` clamped to
// N's span, INSET from its ends.
inline ew_point land(const ew_rect &o, const ew_rect &n, side_t s,
                     ew_point t, double along, double inset) {
  switch (s) {
    case RIGHT:
      return {clampd(n.x + (t.x - o.r()), n.x, n.r() - 1),
              inset_clamp(along, n.y, n.b(), inset)};
    case LEFT:
      return {clampd(n.r() - (o.x - t.x), n.x, n.r() - 1),
              inset_clamp(along, n.y, n.b(), inset)};
    case BOTTOM:
      return {inset_clamp(along, n.x, n.r(), inset),
              clampd(n.y + (t.y - o.b()), n.y, n.b() - 1)};
    case TOP:
      return {inset_clamp(along, n.x, n.r(), inset),
              clampd(n.b() - (o.y - t.y), n.y, n.b() - 1)};
    default:
      return t;
  }
}

// target: true, with OUT set, when the motion should land at OUT instead.
inline bool target(const std::vector<ew_rect> &outs, ew_point c, double dx,
                   double dy, bool proportional, double inset,
                   ew_point &out) {
  const ew_rect *o = nullptr;
  for (const auto &r : outs)
    if (r.contains(c.x, c.y)) { o = &r; break; }
  if (!o) return false;

  const ew_point t{c.x + dx, c.y + dy};
  const side_t s = side_left(*o, t);
  if (s == NONE) return false;

  std::vector<const ew_rect *> nb;
  for (const auto &r : outs)
    if (&r != o && touches(*o, r, s)) nb.push_back(&r);
  if (nb.empty()) return false;        // the outer boundary: not ours

  const bool vert = s == LEFT || s == RIGHT;   // the edge runs vertically
  const double along = vert ? t.y : t.x;

  if (proportional && nb.size() == 1) {
    const ew_rect &n = *nb[0];
    const double f = vert ? (t.y - o->y) / o->h : (t.x - o->x) / o->w;
    out = land(*o, n, s, t, vert ? n.y + f * n.h : n.x + f * n.w, inset);
    return true;
  }

  // The straight warp acts only where nothing is beyond the edge: a target
  // already on an output is the layout's own crossing, and stays so.
  for (const auto &r : outs)
    if (r.contains(t.x, t.y)) return false;

  // The nearest neighbour along the edge.
  const ew_rect *best = nullptr;
  double best_d = 0;
  for (const auto *n : nb) {
    const double lo = vert ? n->y : n->x;
    const double hi = vert ? n->b() : n->r();
    const double d = along < lo ? lo - along : (along >= hi ? along - hi : 0);
    if (!best || d < best_d) { best = n; best_d = d; }
  }
  out = land(*o, *best, s, t, along, inset);
  return true;
}

}  // namespace edge_warp
