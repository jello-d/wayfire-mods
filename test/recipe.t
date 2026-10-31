#!/bin/sh
# recipe.t - every patch the build recipe applies must exist, and every patch in
# patches/ must be referenced by the recipe (a renamed/typo'd/orphaned patch is
# caught before a real build fails cryptically). The plugins tree must be a
# buildable meson project.
. "$(dirname "$0")/harness_lib"
harness_init recipe
_r=$HERE/libexec/build-wayfire
[ -f "$_r" ] || fail "no libexec/build-wayfire"
for _p in "$HERE"/patches/*.patch; do
  [ -e "$_p" ] || continue
  _b=$(basename "$_p" .patch)
  grep -qF "$_b" "$_r" || fail "patch $_b not referenced by the recipe"
done
[ -f "$HERE/plugins/meson.build" ] || fail "plugins/meson.build missing"
# the recipe names each upstream ref var; refs must define them
. "$HERE/refs"
for _v in WLROOTS_REF WAYFIRE_REF WCM_REF PLUGINS_EXTRA_REF FILTERS_REF \
          WF_INFO_REF PIXDECOR_REF; do
  eval "_val=\${$_v:-}"
  [ -n "$_val" ] || fail "refs does not define $_v"
done
pass
