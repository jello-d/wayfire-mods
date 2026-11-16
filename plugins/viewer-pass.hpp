#pragma once

// Shared by toplevel-capture and enclave-mark through wf::shared_data (the
// key is this type's name, so both plugins must include this one header).
//
// While `depth` is above zero, a capture for VIEWERS is collecting what to
// draw (a screen an enclave shares), and the enclave mark draws nothing into
// it: the mark is a cue for the person at this desk, and in a share it would
// show everyone watching the enclave's name. The output's own rendering is
// unaffected, since it collects its draw list at other times.
namespace wf
{
namespace enclave_capture
{
struct viewer_pass_t
{
    int depth = 0;
};
}
}
