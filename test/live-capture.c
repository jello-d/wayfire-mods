/*
 * live-capture: drive window capture the way a screen-sharing portal does,
 * on the compositor named by WAYLAND_DISPLAY. For test/live.t.
 *
 *   live-capture list             one "toplevel <app_id>" line per window in
 *                                 every ext_foreign_toplevel_list_v1 this
 *                                 client is offered, then "lists <n>"
 *   live-capture capture <app_id> capture that window once over
 *                                 ext-image-copy-capture with an shm buffer;
 *                                 prints "captured <w>x<h> <r> <g> <b>" (the
 *                                 centre pixel) or "stopped" / "failed"
 *   live-capture stream <app_id> <n>
 *                                 keep ONE session and capture n frames in
 *                                 a row, as a portal does; a frame after the
 *                                 first comes only when the window changes.
 *                                 Prints "streamed <n> retries <r>", r being
 *                                 the frames redone for a size change.
 *
 * Exit: 0 captured or listed, 3 stopped/failed/not found, 1 setup error.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

#define MAX_TOPLEVELS 64
#define REASON_CONSTRAINTS \
  EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS

static struct wl_shm *shm;
static struct ext_foreign_toplevel_list_v1 *lists[8];
static int n_lists;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *source_mgr;
static struct ext_image_copy_capture_manager_v1 *copy_mgr;

static struct {
  struct ext_foreign_toplevel_handle_v1 *handle;
  char app_id[256];
} toplevels[MAX_TOPLEVELS];
static int n_toplevels;

static uint32_t buf_w, buf_h, shm_format = UINT32_MAX;
static int session_done, session_stopped, frame_ready, frame_failed;
static uint32_t fail_reason;
/* Bumped on every session `done`: new constraints can arrive BEFORE the
 * frame that no longer fits them fails, so a retry must not wait for one. */
static int constraints_gen, buffer_gen;

static void global_add(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
  (void)data; (void)version;
  if (!strcmp(iface, wl_shm_interface.name))
    shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  else if (!strcmp(iface, ext_foreign_toplevel_list_v1_interface.name)
           && n_lists < 8)
    lists[n_lists++] = wl_registry_bind(reg, name,
      &ext_foreign_toplevel_list_v1_interface, 1);
  else if (!strcmp(iface,
      ext_foreign_toplevel_image_capture_source_manager_v1_interface.name))
    source_mgr = wl_registry_bind(reg, name,
      &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
  else if (!strcmp(iface, ext_image_copy_capture_manager_v1_interface.name))
    copy_mgr = wl_registry_bind(reg, name,
      &ext_image_copy_capture_manager_v1_interface, 1);
}

static void global_remove(void *d, struct wl_registry *r, uint32_t n)
{
  (void)d; (void)r; (void)n;
}

static const struct wl_registry_listener registry_listener = {
  global_add, global_remove,
};

static void h_closed(void *d, struct ext_foreign_toplevel_handle_v1 *h)
{ (void)d; (void)h; }
static void h_done(void *d, struct ext_foreign_toplevel_handle_v1 *h)
{ (void)d; (void)h; }
static void h_title(void *d, struct ext_foreign_toplevel_handle_v1 *h,
                    const char *t)
{ (void)d; (void)h; (void)t; }
static void h_app_id(void *d, struct ext_foreign_toplevel_handle_v1 *h,
                     const char *app_id)
{
  (void)h;
  long i = (long)d;
  snprintf(toplevels[i].app_id, sizeof(toplevels[i].app_id), "%s", app_id);
}
static void h_identifier(void *d, struct ext_foreign_toplevel_handle_v1 *h,
                         const char *id)
{ (void)d; (void)h; (void)id; }

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
  h_closed, h_done, h_title, h_app_id, h_identifier,
};

static void l_toplevel(void *d, struct ext_foreign_toplevel_list_v1 *l,
                       struct ext_foreign_toplevel_handle_v1 *handle)
{
  (void)d; (void)l;
  if (n_toplevels >= MAX_TOPLEVELS)
    return;
  toplevels[n_toplevels].handle = handle;
  ext_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener,
                                              (void *)(long)n_toplevels);
  n_toplevels++;
}
static void l_finished(void *d, struct ext_foreign_toplevel_list_v1 *l)
{ (void)d; (void)l; }

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
  l_toplevel, l_finished,
};

static void s_buffer_size(void *d, struct ext_image_copy_capture_session_v1 *s,
                          uint32_t w, uint32_t h)
{ (void)d; (void)s; buf_w = w; buf_h = h; }
static void s_shm_format(void *d, struct ext_image_copy_capture_session_v1 *s,
                         uint32_t f)
{
  (void)d; (void)s;
  if (shm_format == UINT32_MAX || f == WL_SHM_FORMAT_ARGB8888)
    shm_format = f;
}
static void s_dmabuf_device(void *d,
                            struct ext_image_copy_capture_session_v1 *s,
                            struct wl_array *a)
{ (void)d; (void)s; (void)a; }
static void s_dmabuf_format(void *d,
                            struct ext_image_copy_capture_session_v1 *s,
                            uint32_t f, struct wl_array *m)
{ (void)d; (void)s; (void)f; (void)m; }
static void s_done(void *d, struct ext_image_copy_capture_session_v1 *s)
{ (void)d; (void)s; session_done = 1; constraints_gen++; }
static void s_stopped(void *d, struct ext_image_copy_capture_session_v1 *s)
{ (void)d; (void)s; session_stopped = 1; }

static const struct ext_image_copy_capture_session_v1_listener session_listener
  = { s_buffer_size, s_shm_format, s_dmabuf_device, s_dmabuf_format, s_done,
      s_stopped };

static void f_transform(void *d, struct ext_image_copy_capture_frame_v1 *f,
                        uint32_t t)
{ (void)d; (void)f; (void)t; }
static void f_damage(void *d, struct ext_image_copy_capture_frame_v1 *f,
                     int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)d; (void)f; (void)x; (void)y; (void)w; (void)h; }
static void f_time(void *d, struct ext_image_copy_capture_frame_v1 *f,
                   uint32_t hi, uint32_t lo, uint32_t ns)
{ (void)d; (void)f; (void)hi; (void)lo; (void)ns; }
static void f_ready(void *d, struct ext_image_copy_capture_frame_v1 *f)
{ (void)d; (void)f; frame_ready = 1; }
static void f_failed(void *d, struct ext_image_copy_capture_frame_v1 *f,
                     uint32_t reason)
{ (void)d; (void)f; fail_reason = reason; frame_failed = 1; }

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
  f_transform, f_damage, f_time, f_ready, f_failed,
};

static int do_list(void)
{
  for (int i = 0; i < n_toplevels; i++)
    printf("toplevel %s\n", toplevels[i].app_id);
  printf("lists %d\n", n_lists);
  return 0;
}

/* Wait for the session's constraints; 0 when usable, 3 when stopped. */
static int wait_session(struct wl_display *display)
{
  while (!session_done && !session_stopped)
    if (wl_display_dispatch(display) < 0)
      return 1;
  if (session_stopped || !buf_w || shm_format == UINT32_MAX) {
    printf("stopped\n");
    return 3;
  }
  return 0;
}

static unsigned char *pixels;
static size_t stride;

/* An shm buffer for the session's current constraints. */
static struct wl_buffer *make_buffer(void)
{
  buffer_gen = constraints_gen;
  stride = buf_w * 4;
  size_t size = stride * buf_h;
  int fd = memfd_create("live-capture", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, size) < 0)
    return NULL;
  pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, buf_w, buf_h,
                                                       stride, shm_format);
  wl_shm_pool_destroy(pool);
  close(fd);
  return buffer;
}

/* One capture; 0 ready, 2 constraints changed (retry), 3 failed/stopped. */
static int capture_once(struct wl_display *display,
                        struct ext_image_copy_capture_session_v1 *session,
                        struct wl_buffer *buffer)
{
  frame_ready = frame_failed = 0;
  struct ext_image_copy_capture_frame_v1 *frame =
    ext_image_copy_capture_session_v1_create_frame(session);
  ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, NULL);
  ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
  ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, buf_w, buf_h);
  ext_image_copy_capture_frame_v1_capture(frame);
  while (!frame_ready && !frame_failed && !session_stopped)
    if (wl_display_dispatch(display) < 0)
      return 1;
  ext_image_copy_capture_frame_v1_destroy(frame);
  if (frame_failed && !session_stopped
      && fail_reason == REASON_CONSTRAINTS)
    return 2;
  if (!frame_ready) {
    printf(session_stopped ? "stopped\n" : "failed %u\n", fail_reason);
    return 3;
  }
  return 0;
}

static int do_capture(struct wl_display *display, const char *app_id,
                      int frames)
{
  int i;
  for (i = 0; i < n_toplevels; i++)
    if (!strcmp(toplevels[i].app_id, app_id))
      break;
  if (i == n_toplevels || !source_mgr || !copy_mgr) {
    printf("not found\n");
    return 3;
  }
  struct ext_image_capture_source_v1 *source =
    ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
      source_mgr, toplevels[i].handle);
  struct ext_image_copy_capture_session_v1 *session =
    ext_image_copy_capture_manager_v1_create_session(copy_mgr, source, 0);
  ext_image_copy_capture_session_v1_add_listener(session, &session_listener,
                                                 NULL);
  int rc = wait_session(display);
  if (rc)
    return rc;
  struct wl_buffer *buffer = make_buffer();
  int retries = 0;
  for (int k = 0; k < frames; ) {
    rc = capture_once(display, session, buffer);
    if (rc == 2 && retries++ < 3) {
      /* As a portal does: the window resized, so reallocate to the new
       * constraints, waiting for them only if they have not arrived yet.
       * Not counted as a frame. */
      while (constraints_gen == buffer_gen && !session_stopped)
        if (wl_display_dispatch(display) < 0)
          return 1;
      if (session_stopped) {
        printf("stopped\n");
        return 3;
      }
      wl_buffer_destroy(buffer);
      buffer = make_buffer();
      continue;
    }
    if (rc)
      return rc == 2 ? 3 : rc;
    k++;
  }
  if (frames > 1) {
    printf("streamed %d retries %d\n", frames, retries);
    return 0;
  }
  /* ARGB8888/XRGB8888 are little-endian in memory: B, G, R, A. */
  unsigned char *c = pixels + (buf_h / 2) * stride + (buf_w / 2) * 4;
  printf("captured %ux%u %u %u %u\n", buf_w, buf_h, c[2], c[1], c[0]);
  return 0;
}

int main(int argc, char **argv)
{
  int stream = argc == 4 && !strcmp(argv[1], "stream");
  if (argc < 2 || (strcmp(argv[1], "list") && argc != 3 && !stream)) {
    fprintf(stderr, "usage: live-capture list | capture <app_id>"
            " | stream <app_id> <n>\n");
    return 1;
  }
  struct wl_display *display = wl_display_connect(NULL);
  if (!display) {
    fprintf(stderr, "live-capture: cannot connect\n");
    return 1;
  }
  struct wl_registry *reg = wl_display_get_registry(display);
  wl_registry_add_listener(reg, &registry_listener, NULL);
  wl_display_roundtrip(display);
  for (int i = 0; i < n_lists; i++)
    ext_foreign_toplevel_list_v1_add_listener(lists[i], &list_listener, NULL);
  wl_display_roundtrip(display);
  wl_display_roundtrip(display);
  if (!strcmp(argv[1], "list"))
    return do_list();
  return do_capture(display, argv[2], stream ? atoi(argv[3]) : 1);
}
