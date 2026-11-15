/*
 * live-pwconsume <node id> <seconds>: connect a video consumer to a PipeWire
 * node and count the buffers that arrive, the way a browser does after a
 * portal ScreenCast Start. Prints one line:
 *
 *   consumed <n> <W>x<H> <format id>   buffers received in the window, and
 *                         the format NEGOTIATED, so a caller can tell the
 *                         stream it asked for from any other source
 *   refused <msg>         every link attempt was refused, with the last
 *                         reason ("no more output formats" is the 0.8.1 bug)
 *   unlinked              no format was agreed and no link was refused
 *   error <state> <msg>   the stream itself failed
 *
 * WHY IT EXISTS: a stream that is CREATED is not a stream anything can READ.
 * xdg-desktop-portal-wlr 0.8.1 offers a window cast at framerate 0, and on
 * pipewire 1.6 that negotiates to "no more output formats" with every
 * consumer, so Start succeeded and every share failed (2026-11-15, an
 * enclave's Meet). The params below are a browser's: any raw size and
 * format, framerate and maxFramerate up to 60, which is what refused it.
 *
 * IT LINKS ITSELF, through the link factory (what pw-link does), to the node
 * id the portal returned, retrying every 200ms: an attempt made before our
 * own port is registered is refused with "unknown input port", which is a
 * race and not a verdict. No session manager decides anything. That is
 * deliberate. Left to WirePlumber, a raw client is linked by POLICY: with
 * no target it took the DEFAULT video source, which on a laptop is the
 * camera (measured: NV12 1920x1080, refused, and reported as the share
 * failing), and with a target it answered "defined target not found" for a
 * node plainly in the graph. A browser never meets either, since it reads
 * the share through the portal's restricted remote. The question here is
 * only whether the two ENDS agree on a format, and a direct link asks
 * exactly that.
 */
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PWCONSUME_TRACE=1 narrates the stream and the link on stderr. */
static int trace;
#define TRACE(...) do { if (trace) fprintf(stderr, __VA_ARGS__); } while (0)

struct ctx {
	struct pw_main_loop *loop;
	struct pw_stream *stream;
	uint32_t source;
	struct pw_proxy *link;
	int frames;
	int failed;
	struct spa_video_info_raw fmt;
	int have_fmt;
	int link_failed;
	int paused;
	char refused[128];
};

static void on_param(void *data, uint32_t id, const struct spa_pod *param)
{
	struct ctx *c = data;
	if (param == NULL || id != SPA_PARAM_Format)
		return;
	if (spa_format_video_raw_parse(param, &c->fmt) >= 0)
		c->have_fmt = 1;
}

static void on_process(void *data)
{
	struct ctx *c = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(c->stream);
	if (b == NULL)
		return;
	c->frames++;
	pw_stream_queue_buffer(c->stream, b);
}

static void on_link_error(void *data, int seq, int res, const char *message)
{
	struct ctx *c = data;
	(void)seq;
	TRACE("link error %d %s\n", res, message);
	c->link_failed = 1;
	snprintf(c->refused, sizeof(c->refused), "%s",
		message ? message : "-");
}

static void on_link_info(void *data, const struct pw_link_info *info)
{
	(void)data;
	TRACE("link %u -> %u state %s %s\n", info->output_node_id,
		info->input_node_id, pw_link_state_as_string(info->state),
		info->error ? info->error : "");
}

static const struct pw_proxy_events link_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.error = on_link_error,
};

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.info = on_link_info,
};

static struct spa_hook link_proxy_hook, link_hook;

static void on_state(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct ctx *c = data;
	(void)old;
	TRACE("state %s node %u\n", pw_stream_state_as_string(state),
		pw_stream_get_node_id(c->stream));
	if (state == PW_STREAM_STATE_PAUSED)
		c->paused = 1;
	if (state == PW_STREAM_STATE_ERROR) {
		printf("error %s %s\n", pw_stream_state_as_string(state),
			error ? error : "-");
		c->failed = 1;
		pw_main_loop_quit(c->loop);
	}
}

/* Link the source to our node. The link factory can answer before our port
 * is registered ("unknown input port"), so a failed attempt is dropped and
 * tried again on the next tick rather than ending the run. */
static void on_tick(void *data, uint64_t expirations)
{
	struct ctx *c = data;
	(void)expirations;
	if (c->link != NULL && c->link_failed) {
		spa_hook_remove(&link_proxy_hook);
		spa_hook_remove(&link_hook);
		pw_proxy_destroy(c->link);
		c->link = NULL;
		c->link_failed = 0;
	}
	if (!c->paused || c->link != NULL || c->have_fmt)
		return;
	char out[16], in[16];
	snprintf(out, sizeof(out), "%u", c->source);
	snprintf(in, sizeof(in), "%u", pw_stream_get_node_id(c->stream));
	c->link = pw_core_create_object(pw_stream_get_core(c->stream),
		"link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
		&SPA_DICT_INIT_ARRAY(((struct spa_dict_item[]){
			{ PW_KEY_LINK_OUTPUT_NODE, out },
			{ PW_KEY_LINK_INPUT_NODE, in },
			{ PW_KEY_OBJECT_LINGER, "false" } })), 0);
	TRACE("link try %s -> %s\n", out, in);
	if (c->link != NULL) {
		pw_proxy_add_listener(c->link, &link_proxy_hook,
			&link_proxy_events, c);
		pw_proxy_add_object_listener(c->link, &link_hook,
			&link_events, c);
	}
}

static void on_timeout(void *data, uint64_t expirations)
{
	(void)expirations;
	pw_main_loop_quit(((struct ctx *)data)->loop);
}

static const struct pw_stream_events events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state,
	.param_changed = on_param,
	.process = on_process,
};

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: live-pwconsume <node id> <seconds>\n");
		return 2;
	}
	uint32_t node = (uint32_t)strtoul(argv[1], NULL, 10);
	int secs = atoi(argv[2]);
	struct ctx c = {0};
	c.source = node;
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[1];

	trace = getenv("PWCONSUME_TRACE") != NULL;
	pw_init(&argc, &argv);
	c.loop = pw_main_loop_new(NULL);
	c.stream = pw_stream_new_simple(pw_main_loop_get_loop(c.loop),
		"live-pwconsume",
		pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Screen",
			PW_KEY_NODE_AUTOCONNECT, "false", NULL),
		&events, &c);
	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
			SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
			SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
			SPA_VIDEO_FORMAT_RGBA),
		SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
			&SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(1, 1),
			&SPA_RECTANGLE(16384, 16384)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1),
			&SPA_FRACTION(60, 1)),
		SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1),
			&SPA_FRACTION(60, 1)));
	if (pw_stream_connect(c.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS,
			params, 1) < 0) {
		printf("error connect -\n");
		return 0;
	}
	struct spa_source *t = pw_loop_add_timer(
		pw_main_loop_get_loop(c.loop), on_timeout, &c);
	struct timespec ts = { secs, 0 };
	pw_loop_update_timer(pw_main_loop_get_loop(c.loop), t, &ts, NULL, false);
	struct spa_source *k = pw_loop_add_timer(
		pw_main_loop_get_loop(c.loop), on_tick, &c);
	struct timespec tk = { 0, 200000000 };
	pw_loop_update_timer(pw_main_loop_get_loop(c.loop), k, &tk, &tk, false);
	pw_main_loop_run(c.loop);
	if (!c.failed && !c.have_fmt && c.refused[0])
		printf("refused %s\n", c.refused);
	else if (!c.failed && !c.have_fmt)
		printf("unlinked\n");
	else if (!c.failed)
		printf("consumed %d %ux%u %u\n", c.frames, c.fmt.size.width,
			c.fmt.size.height, (unsigned)c.fmt.format);
	pw_stream_destroy(c.stream);
	pw_main_loop_destroy(c.loop);
	pw_deinit();
	return 0;
}
