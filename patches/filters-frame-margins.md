# filters-frame-margins.patch

A patch to soreau/filters (pinned at `FILTERS_REF`, currently
`33f477847085e05a1682e0747a728aa1e817308e`) that publishes the server-side
decoration extent to filter shaders as a new `frame_margins` uniform.

Applied by `modules/compositor` (the filters step inlines gate+fetch+patch+build+
mark; the build pin folds in this patch's content, so editing it forces a
filters rebuild). filters is a plugin `.so`, so picking up a rebuild needs a
compositor restart (`RTLD_GLOBAL`, same as any Wayfire plugin).

## What it changes

One addition in the view transformer's `render()` (`wf_filters`), right after
it sets the existing `margins` uniform: it reads the toplevel's authoritative
decoration margins and publishes them as a second uniform.

```cpp
auto fm = toplevel->toplevel()->current().margins;
this->self->shader->uniform4f("frame_margins",
    glm::vec4{(float)fm.left, (float)fm.bottom, (float)fm.right, (float)fm.top});
```

Packing matches the existing `margins` convention: `x=left, y=bottom, z=right,
w=top` (pixels). Purely additive: any shader that does not declare
`frame_margins` is unaffected (`uniform4f` on a missing location is a no-op).

## Why

A rendered decorated view has three concentric rectangles:

```
outer      = full rendered region (shadow edge)
silhouette = the visible window (frame outer edge)   outer  -> silhouette = shadow
content    = the client surface  (frame inner edge)  silhouette -> content = frame
```

Filters fall into three scopes, and they want different boundaries:

- content-only (invert, monochrome): operate inside `content`.
- whole (crt, passthrough): ignore boundaries.
- silhouette (rounded-corners, border): operate on the `silhouette` edge.

Upstream exposes exactly one hint, `margins`, derived from
`surface_root_bbox` vs `get_geometry()` plus pixdecor's shadow data. That
conflates shadow and frame into a single number, and crucially it reads ~0 for
Wayfire's built-in `decoration` plugin: that decorator draws the frame as a
subsurface and expands the toplevel geometry to match, so `surface_root` and
`get_geometry()` coincide and the subtraction is zero (then padded to 2px). So
`margins` cannot tell a content shader where the frame is on a built-in-
decorated window -- and the `invert` shader ended up inverting the titlebar,
destroying the focus cue (active vs inactive titlebar colour).

The real frame extent is not derivable from the rendered texture; it lives on
the toplevel state as `current().margins` and is shell-independent (it is also
`{0,0,0,0}` for a fullscreen window, so a fullscreen invert has no dead strip).
Publishing it as a distinct uniform lets each shader pick its own boundary:
content shaders inset by `margins + frame_margins`; silhouette shaders keep
using `margins` alone and are unchanged.

## Why additive (not a change to `margins`)

Reusing `margins` for the frame would have regressed `rounded-corners` /
`border`: with a nonzero frame they would round/border the *content* rect,
leaving the square decoration frame around it (rounded content inside square
chrome). Keeping `margins` as-is and adding `frame_margins` fixes the content
case with zero change to the silhouette shaders.

## What consumes it here

`deploy/filters/shaders/invert` reads both uniforms and insets its inversion by
`margins + frame_margins`, so it covers the client content only. No other
shader in `deploy/filters/shaders/` declares `frame_margins`, so all are
unchanged.

## Upstream case

This is a clean candidate to submit to soreau/filters:

- Additive: a new uniform, no behaviour change for any existing shader.
- Fills a real gap: content-scoped shaders currently cannot avoid the SSD
  decoration on the built-in decorator (the most common Wayfire setup), only
  on pixdecor. The `invert` shader wanting to spare the titlebar is the
  motivating case; `monochrome` has the same need.
- Small and self-contained: ~3 lines in the transformer, plus updating the
  bundled `invert` shader to use it.

If submitted, the commit message is essentially this file's "Why" section.

## Verified

Confirmed working on manifold (2026-07-22): with per-window invert active, the
client content inverts while the server-side titlebar/border keep their real
active/inactive colours, so focus stays legible.

One on-box correction along the way: the vertical (top vs bottom) assignment in
the `invert` shader's content test is REVERSED from `rounded-corners`. In this
render path `uvpos.y == 0` is the window BOTTOM, so the top margin (`m.w`, the
titlebar) is the UPPER bound and the bottom margin (`m.y`) the lower. The first
build used rounded-corners' ordering and inverted the titlebar while sparing
the bottom; swapping the two vertical bounds fixed it. rounded-corners' own
symmetric rounding masks this, which is why its ordering looked authoritative.
