// atlas-anim.hpp
//
// Shared open/close animation driver for atlas's overview overlays (ring and
// wall). It owns the animation clock and the per-output frame pump; the overlay
// node reads progress() to interpolate its own layout, and the plugin calls
// open() once and close() to run the exit.
//
// One copy of the lifecycle, so the tricky parts live in a single place:
//   - progress() runs 0 -> 1 on open. close() REVERSES in place: duration_t::
//     reverse keeps the current value and flows it back to 0, so hitting the
//     exit mid-open just turns the motion around, no jump (and a barely-opened
//     overview closes proportionally fast).
//   - a static overlay never repaints itself, so an OUTPUT_EFFECT_PRE hook per
//     output damages the whole output every frame while the clock runs. When
//     the close settles, on_done fires EXACTLY once.
//   - on_done fires synchronously from inside that render hook, so it MUST NOT
//     destroy this driver -- that would rem_effect the very hook we are in. The
//     caller only arms an idle from on_done and tears down there (see atlas.cpp
//     exit_mode). The destructor is the instant teardown for the cancel path
//     (desktop lock), which never animates.

#pragma once

#include <algorithm>
#include <functional>
#include <list>

#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/config/option-wrapper.hpp>

namespace atlas
{
class anim_driver_t
{
  public:
    explicit anim_driver_t(int anim_ms) :
        anim(wf::create_option<int>(std::max(1, anim_ms)))
    {}

    anim_driver_t(const anim_driver_t&) = delete;
    anim_driver_t& operator =(const anim_driver_t&) = delete;

    ~anim_driver_t()
    {
        for (auto& r : hooks)
        {
            r.wo->render->rem_effect(&r.hook);
        }

        hooks.clear();
    }

    // Begin the open animation (progress 0 -> 1) and start pumping frames.
    void open()
    {
        register_hooks();
        anim.start();
        damage_all();
    }

    // Install the frame pump WITHOUT running an animation (progress stays at
    // its settled 1.0). For a driver used as a re-triggerable tween -- arm()
    // once, then restart() on each new target.
    void arm()
    {
        register_hooks();
    }

    // Re-run the 0 -> 1 animation from the start (hooks already installed).
    // Used by the wall's drag preview to tween the grabbed window between cells
    // each time the cursor crosses into a new workspace.
    void restart()
    {
        anim.start();
        damage_all();
    }

    // Reverse toward 0 from the current progress; on_done fires once when it
    // settles. No-op if a close is already running.
    void close(std::function<void()> on_done_cb)
    {
        if (is_closing)
        {
            return;
        }

        on_done    = std::move(on_done_cb);
        is_closing = true;
        anim.reverse();
        damage_all();
    }

    // 0 at the pre-open layout (real window positions / the single current
    // workspace), 1 fully open (the grid). Read by the overlay node's render().
    double progress() const
    {
        return anim.progress();
    }

    bool closing() const
    {
        return is_closing;
    }

  private:
    wf::animation::duration_t anim;
    bool is_closing = false;
    bool done_fired = false;
    std::function<void()> on_done;

    // std::list keeps each hook's address stable for rem_effect.
    struct hook_reg
    {
        wf::output_t *wo;
        wf::effect_hook_t hook;
    };
    std::list<hook_reg> hooks;

    void damage_all()
    {
        for (auto& r : hooks)
        {
            r.wo->render->damage_whole();
        }
    }

    void register_hooks()
    {
        for (auto *wo : wf::get_core().output_layout->get_outputs())
        {
            hooks.push_back({wo, {}});
            auto& r = hooks.back();
            r.hook = [this] () { on_frame(); };
            wo->render->add_effect(&r.hook, wf::OUTPUT_EFFECT_PRE);
        }
    }

    void on_frame()
    {
        if (anim.running())
        {
            damage_all();
            return;
        }

        if (is_closing && !done_fired)
        {
            done_fired = true;
            if (on_done)
            {
                on_done();
            }
        }
    }
};
} // namespace atlas
