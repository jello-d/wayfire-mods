/*
 * live-activate: xdg-activation-v1 from both ends, for test/live.t, on the
 * compositor named by WAYLAND_DISPLAY. Driven over stdin so the test decides
 * when each step happens.
 *
 *   live-activate kbd             create a virtual keyboard (so the seat has
 *                                 one and focused clients get serials), print
 *                                 "ready", wait to be killed
 *   live-activate token <app_id>  map a window; for each stdin line "surface"
 *                                 or "nosurface", create a token from the
 *                                 latest keyboard-enter serial (with or
 *                                 without this window as its surface) and
 *                                 print "token <string>" (or "token-failed")
 *   live-activate activate <app_id>
 *                                 map a window, print "mapped"; for each
 *                                 stdin line holding a token, ask to activate
 *                                 this window with it and print "requested"
 *
 * Exit 1 on a setup error.
 */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct xdg_wm_base *wm_base;
static struct xdg_activation_v1 *activation;
static struct zwp_virtual_keyboard_manager_v1 *vkbd_mgr;
static struct wl_surface *surface;
static int configured, mapped, token_done;
static uint32_t enter_serial, have_serial;
static char token_str[512];

static void global_add(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
  (void)d; (void)version;
  if (!strcmp(iface, wl_compositor_interface.name))
    compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, wl_shm_interface.name))
    shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  else if (!strcmp(iface, wl_seat_interface.name))
    seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
  else if (!strcmp(iface, xdg_wm_base_interface.name))
    wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
  else if (!strcmp(iface, xdg_activation_v1_interface.name))
    activation = wl_registry_bind(reg, name, &xdg_activation_v1_interface, 1);
  else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
    vkbd_mgr = wl_registry_bind(reg, name,
                                &zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }

static const struct wl_registry_listener registry_listener = {
  global_add, global_remove,
};

static void ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{ (void)d; xdg_wm_base_pong(b, serial); }

static const struct xdg_wm_base_listener wm_base_listener = { ping };

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd,
                     uint32_t size)
{ (void)d; (void)k; (void)f; (void)size; close(fd); }
static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial,
                    struct wl_surface *s, struct wl_array *keys)
{
  (void)d; (void)k; (void)s; (void)keys;
  enter_serial = serial;
  have_serial = 1;
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial,
                    struct wl_surface *s)
{ (void)d; (void)k; (void)serial; (void)s; }
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t,
                  uint32_t key, uint32_t state)
{ (void)d; (void)k; (void)serial; (void)t; (void)key; (void)state; }
static void k_modifiers(void *d, struct wl_keyboard *k, uint32_t serial,
                        uint32_t dep, uint32_t lat, uint32_t lock,
                        uint32_t group)
{ (void)d; (void)k; (void)serial; (void)dep; (void)lat; (void)lock;
  (void)group; }
static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate,
                     int32_t delay)
{ (void)d; (void)k; (void)rate; (void)delay; }

static const struct wl_keyboard_listener keyboard_listener = {
  k_keymap, k_enter, k_leave, k_key, k_modifiers, k_repeat,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
  (void)d;
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
    keyboard = wl_seat_get_keyboard(s);
    wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
  }
}
static void seat_name(void *d, struct wl_seat *s, const char *n)
{ (void)d; (void)s; (void)n; }

static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

/* A 64x64 opaque shm buffer: enough for the window to map. */
static struct wl_buffer *solid_buffer(void)
{
  int w = 64, h = 64, stride = w * 4, size = stride * h;
  int fd = memfd_create("live-activate", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, size) < 0)
    return NULL;
  uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  for (int i = 0; i < w * h; i++)
    px[i] = 0xff808080;
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                  WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  return b;
}

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
  (void)d;
  xdg_surface_ack_configure(xs, serial);
  if (!configured) {
    wl_surface_attach(surface, solid_buffer(), 0, 0);
    wl_surface_commit(surface);
    configured = 1;
  }
}

static const struct xdg_surface_listener xdg_surface_listener = {
  xs_configure,
};

static void map_window(struct wl_display *display, const char *app_id)
{
  surface = wl_compositor_create_surface(compositor);
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
  xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
  struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
  xdg_toplevel_set_app_id(tl, app_id);
  wl_surface_commit(surface);
  while (!configured && wl_display_dispatch(display) >= 0)
    ;
  wl_display_roundtrip(display);
  mapped = 1;
}

static void t_done(void *d, struct xdg_activation_token_v1 *t, const char *s)
{
  (void)d; (void)t;
  snprintf(token_str, sizeof(token_str), "%s", s);
  token_done = 1;
}

static const struct xdg_activation_token_v1_listener token_listener = {
  t_done,
};

static void make_token(struct wl_display *display, int with_surface)
{
  struct xdg_activation_token_v1 *t =
    xdg_activation_v1_get_activation_token(activation);
  xdg_activation_token_v1_add_listener(t, &token_listener, NULL);
  if (have_serial)
    xdg_activation_token_v1_set_serial(t, enter_serial, seat);
  if (with_surface)
    xdg_activation_token_v1_set_surface(t, surface);
  token_done = 0;
  xdg_activation_token_v1_commit(t);
  while (!token_done && wl_display_dispatch(display) >= 0)
    ;
  printf(token_done ? "token %s\n" : "token-failed\n", token_str);
  fflush(stdout);
  xdg_activation_token_v1_destroy(t);
}

/* Serve stdin lines while keeping the connection dispatched. */
static int serve(struct wl_display *display, const char *mode)
{
  char line[600];
  struct pollfd fds[2] = {
    { .fd = wl_display_get_fd(display), .events = POLLIN },
    { .fd = STDIN_FILENO, .events = POLLIN },
  };
  for (;;) {
    wl_display_flush(display);
    if (poll(fds, 2, -1) < 0)
      return 1;
    if ((fds[0].revents & POLLIN) && wl_display_dispatch(display) < 0)
      return 1;
    if (!(fds[1].revents & (POLLIN | POLLHUP)))
      continue;
    if (!fgets(line, sizeof(line), stdin)) {
      fds[1].fd = -1;   /* stdin closed: just keep the window up */
      continue;
    }
    line[strcspn(line, "\n")] = 0;
    wl_display_roundtrip(display);   /* take any new enter serial first */
    if (!strcmp(mode, "token")) {
      make_token(display, strcmp(line, "nosurface") != 0);
    } else {
      xdg_activation_v1_activate(activation, line, surface);
      wl_display_roundtrip(display);
      printf("requested\n");
      fflush(stdout);
    }
  }
}

static int virtual_keyboard(struct wl_display *display)
{
  static const char keymap[] =
    "xkb_keymap { xkb_keycodes { include \"evdev+aliases(qwerty)\" };"
    " xkb_types { include \"complete\" };"
    " xkb_compat { include \"complete\" };"
    " xkb_symbols { include \"pc+us+inet(evdev)\" }; };";
  if (!vkbd_mgr)
    return 1;
  struct zwp_virtual_keyboard_v1 *k =
    zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkbd_mgr, seat);
  int fd = memfd_create("keymap", MFD_CLOEXEC);
  if (fd < 0 || write(fd, keymap, sizeof(keymap)) != (ssize_t)sizeof(keymap))
    return 1;
  zwp_virtual_keyboard_v1_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                 sizeof(keymap));
  wl_display_roundtrip(display);
  printf("ready\n");
  fflush(stdout);
  while (wl_display_dispatch(display) >= 0)
    ;
  return 0;
}

int main(int argc, char **argv)
{
  if (argc < 2 || (strcmp(argv[1], "kbd") && argc != 3)) {
    fprintf(stderr, "usage: live-activate kbd | token <app_id>"
            " | activate <app_id>\n");
    return 1;
  }
  struct wl_display *display = wl_display_connect(NULL);
  if (!display)
    return 1;
  struct wl_registry *reg = wl_display_get_registry(display);
  wl_registry_add_listener(reg, &registry_listener, NULL);
  wl_display_roundtrip(display);
  if (!seat || !compositor || !shm)
    return 1;
  wl_seat_add_listener(seat, &seat_listener, NULL);
  if (!strcmp(argv[1], "kbd"))
    return virtual_keyboard(display);
  if (!wm_base || !activation)
    return 1;
  xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
  wl_display_roundtrip(display);
  map_window(display, argv[2]);
  printf("mapped\n");
  fflush(stdout);
  return serve(display, argv[1]);
}
