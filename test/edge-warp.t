#!/bin/sh
# edge-warp.t - the geometry of plugins/edge-warp, compiled on its own and run
# against a table of cases: when the pointer is carried across a jagged edge,
# where it lands, and every case where it must be left to the layout's own
# clamping (the outer boundary, a corner, a target already on an output).
# No compositor: edge-warp-geom.hpp has no wayfire in it for exactly this.
# The behaviour in a real compositor is test/live.t's edge-warp check.
set -eu
. "$(dirname "$0")/harness_lib"
harness_init edge-warp
command -v c++ >/dev/null 2>&1 || skip "needs c++"

cat > "$T/cases.cpp" <<'CPP'
#include <cstdio>
#include "edge-warp-geom.hpp"

static int fails = 0;
static void expect(const char *name, std::vector<ew_rect> o, ew_point c,
                   double dx, double dy, bool prop, bool want,
                   ew_point at = {0, 0}, double inset = 16) {
  ew_point got{-1, -1};
  const bool r = edge_warp::target(o, c, dx, dy, prop, inset, got);
  if (r != want || (want && (got.x != at.x || got.y != at.y))) {
    std::printf("FAIL %s: got %d (%g,%g), want %d (%g,%g)\n", name, r,
                got.x, got.y, want, at.x, at.y);
    fails++;
  }
}

int main() {
  // A wide output with a short one to its right: the right edge's lower half
  // has nothing beyond it.
  std::vector<ew_rect> a{{0, 0, 1280, 720}, {1280, 0, 640, 360}};
  // Slid to the nearest point, then INSET 16 from the neighbour's end: its
  // last row is its outer edge, where an edge flip would trigger.
  expect("void: carried across, slid in, inset from the far end", a,
         {1279, 600}, 21, 0, false, true, {1300, 343});
  expect("inset 0 lands on the neighbour's very edge", a,
         {1279, 600}, 21, 0, false, true, {1300, 359}, 0);
  expect("shared part: the layout's own crossing", a,
         {1279, 100}, 21, 0, false, false);
  expect("proportional: the same fraction along", a,
         {1279, 600}, 21, 0, true, true, {1300, 300});
  expect("proportional moves the shared part too", a,
         {1279, 100}, 21, 0, true, true, {1300, 50});
  expect("outer boundary: no neighbour that side", a,
         {0, 600}, -5, 0, false, false);
  expect("past a corner: left to the layout", a,
         {1279, 719}, 5, 5, false, false);
  expect("back the other way: an ordinary crossing", a,
         {1280, 100}, -1, 0, false, false);
  expect("a motion inside the output: nothing", a,
         {600, 600}, 10, 10, false, false);

  // The real desk's laptop (2880x1800 at 0,1665) under the U2715H (2560x1440
  // at 320,225): the laptop's top edge left of x=320 has nothing above it.
  std::vector<ew_rect> desk{{0, 1665, 2880, 1800}, {320, 225, 2560, 1440},
                            {2880, 0, 2160, 3840}};
  expect("desk: up off the laptop's top-left corner", desk,
         {100, 1665}, 0, -10, false, true, {336, 1655});
  expect("desk: up where the U2715H is above", desk,
         {1000, 1665}, 0, -10, false, false);

  // A short output BELOW a wide one, centred.
  std::vector<ew_rect> b{{0, 0, 1920, 1080}, {480, 1080, 960, 540}};
  expect("bottom: carried down, slid along", b,
         {100, 1079}, 0, 10, false, true, {496, 1089});
  // A short output to the LEFT.
  std::vector<ew_rect> l{{1000, 0, 1000, 1000}, {0, 0, 1000, 500}};
  expect("left: carried across", l, {1000, 800}, -3, 0, false, true,
         {997, 483});

  // Two short outputs on one side: the nearest takes it, and proportional
  // (which needs exactly one) falls back to the straight warp.
  std::vector<ew_rect> two{{0, 0, 1000, 1000}, {1000, 0, 500, 300},
                           {1000, 700, 500, 300}};
  expect("two neighbours: the nearer (upper)", two,
         {999, 400}, 5, 0, false, true, {1004, 283});
  expect("two neighbours: the nearer (lower)", two,
         {999, 600}, 5, 0, false, true, {1004, 716});
  expect("two neighbours: proportional falls back, shared part", two,
         {999, 100}, 5, 0, true, false);
  expect("two neighbours: proportional falls back, void", two,
         {999, 400}, 5, 0, true, true, {1004, 283});

  // A neighbour shorter than twice the inset: the landing is its middle.
  std::vector<ew_rect> tiny{{0, 0, 100, 100}, {100, 0, 50, 20}};
  expect("a short neighbour caps the inset at half its span", tiny,
         {99, 60}, 5, 0, false, true, {104, 9.5});
  // Proportional landings keep the inset too: 30/720 of 360 is 15, inside
  // the span but within 16 of its outer end.
  expect("proportional near the neighbour's end: inset as well", a,
         {1279, 30}, 21, 0, true, true, {1300, 16});
  expect("proportional clear of the ends: exactly the fraction", a,
         {1279, 360}, 21, 0, true, true, {1300, 180});

  // A gap between outputs is not an edge they share: nothing touches.
  std::vector<ew_rect> gap{{0, 0, 1000, 1000}, {1001, 0, 500, 500}};
  expect("a one-pixel gap is the outer boundary", gap,
         {999, 800}, 5, 0, false, false);
  return fails ? 1 : 0;
}
CPP
_out=$(c++ -std=c++17 -Wall -Wextra -Werror -I "$HERE/plugins" \
  "$T/cases.cpp" -o "$T/cases" 2>&1) || fail "the geometry does not compile:
$_out"
_out=$("$T/cases" 2>&1) || fail "$_out"
pass "21 cases: jagged edges carried, the rest left to the layout"
