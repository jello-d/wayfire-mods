/*
 * live-failtest: an LD_PRELOAD shim for test/live.t's backoff session. It
 * makes wlr_output_test_state() fail for ONE output while a trigger file
 * exists, which is what an output whose connector has just vanished looks
 * like to the compositor (the undock that livelocked wayfire, see
 * patches/wayfire-output-commit-backoff.md):
 *
 *   WFM_FAILTEST=<file>          fail while this file exists
 *   WFM_FAILTEST_OUTPUT=<name>   the output to fail, e.g. HEADLESS-1
 *
 * A state that switches the output OFF still passes, as it would for a gone
 * connector. Every call it fails is counted in <file>.count (one byte per
 * failure, appended), so the test can measure the retry rate without
 * parsing the compositor's log.
 *
 * Interposes because wayfire calls into libwlroots through its PLT.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_output.h>

typedef bool (*test_fn)(struct wlr_output *, const struct wlr_output_state *);

static bool switches_off(const struct wlr_output_state *state)
{
	return (state->committed & WLR_OUTPUT_STATE_ENABLED) && !state->enabled;
}

bool wlr_output_test_state(struct wlr_output *output,
		const struct wlr_output_state *state)
{
	static test_fn real;
	const char *file = getenv("WFM_FAILTEST");
	const char *name = getenv("WFM_FAILTEST_OUTPUT");
	char count[4096];
	int fd;

	if (!real) {
		real = (test_fn)dlsym(RTLD_NEXT, "wlr_output_test_state");
	}
	if (file && name && output->name && strcmp(output->name, name) == 0
			&& !switches_off(state) && access(file, F_OK) == 0) {
		snprintf(count, sizeof(count), "%s.count", file);
		fd = open(count, O_WRONLY | O_APPEND | O_CREAT, 0600);
		if (fd >= 0) {
			(void)!write(fd, "x", 1);
			close(fd);
		}
		return false;
	}
	return real(output, state);
}
