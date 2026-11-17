#pragma once

// Shared by idle-audit (the only writer) and enclave-mark (the reader), so a
// window holding idle off can be marked on sight (tackup TW1). Both plugins
// must include this one header: the data is found by name and dynamic_cast,
// and the signal by its type.
//
// idle-audit keeps, on each TOPLEVEL whose surface (or a subsurface or popup
// of it) holds a client idle inhibitor, how many it holds, and emits
// changed_signal on core whenever a window starts or stops holding. A holder
// with no toplevel (a layer surface, a bare surface) carries nothing here.
//
// The data is present exactly while the count is above zero, so a reader that
// loads after idle-audit sees the current holders without having heard a
// signal, and one that loads before hears every change.
#include <wayfire/object.hpp>
#include <wayfire/view.hpp>

namespace wf
{
namespace idle_holder
{
inline constexpr const char *DATA_NAME = "idle-holder";

struct holder_t : public wf::custom_data_t
{
    int count = 0;
};

// Emitted on core when `view` starts or stops holding idle off.
struct changed_signal
{
    wayfire_view view;
};

inline bool holds(wayfire_view view)
{
    auto d = view ? view->get_data<holder_t>(DATA_NAME) : nullptr;
    return d && (d->count > 0);
}
}
}
