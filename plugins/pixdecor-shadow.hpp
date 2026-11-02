#pragma once

// Vendored from pixdecor (src/deco-subsurface.hpp, unchanged from 76a0e89
// through d74909d): pixdecor records its drop-shadow thickness as this
// custom_data_t on the VIEW, under the name below, and EXPANDS THE VIEW
// GEOMETRY TO INCLUDE THE SHADOW, so get_geometry() is the shadow's outer
// edge rather than the visible window. Plugins that need the visible edge
// strip it with pixdecor_shadow() below. The ONE copy, shared by every plugin
// here that needs it, so the vendoring cannot drift between them.
//
// The type must match pixdecor's byte-for-byte: same name at global scope,
// same wf::custom_data_t base, same single decoration_margins_t member,
// because Wayfire dlopens plugins RTLD_GLOBAL (plugin-loader.cpp: "RTLD_GLOBAL
// is required for RTTI/dynamic_cast across plugins"), so the typeinfo unifies
// across the .so files and get_data<>()'s dynamic_cast finds pixdecor's
// stored object, then reads margins at the matching offset. If pixdecor
// renames or moves the type or the key, the read degrades to null (the shadow
// gap returns), never a crash: re-sync this vendoring then.

#include <wayfire/object.hpp>
#include <wayfire/toplevel.hpp>
#include <wayfire/view.hpp>

static const char *const SHADOW_MARGIN_DATA = "wf-decoration-shadow-margin";

class wf_shadow_margin_t : public wf::custom_data_t
{
  public:
    wf::decoration_margins_t get_margins()
    {
        return margins;
    }

    void set_margins(wf::decoration_margins_t margins)
    {
        this->margins = margins;
    }

  private:
    wf::decoration_margins_t margins = {0, 0, 0, 0};
};

// The shadow pixdecor added around @view, or all zeros: an undecorated view,
// or a decorator that does not publish it, has no shadow in its geometry.
inline wf::decoration_margins_t pixdecor_shadow(wayfire_view view)
{
    if (auto sm = view ? view->get_data<wf_shadow_margin_t>(SHADOW_MARGIN_DATA)
        : nullptr)
    {
        return sm->get_margins();
    }

    return {0, 0, 0, 0};
}
