/*
 * live-inhibit: take a zwp_idle_inhibitor_v1, for test/live.t's idle-audit
 * check, on the compositor named by WAYLAND_DISPLAY.
 *
 *   live-inhibit window <app_id>  map a window, take an inhibitor on it
 *   live-inhibit bare             take one on a surface with no role, so
 *                                 there is no window behind it at all
 *   live-inhibit early <app_id>   take one on the bare surface FIRST, then
 *                                 map it as a window: the holder has no
 *                                 window when it is taken, only later
 *
 * Prints "inhibiting" once the request has reached the compositor. Each stdin
 * line "release" destroys the inhibitor (the window stays) and prints
 * "released". Exits on stdin EOF, or when killed, which is the disconnect
 * that must release whatever is still held.
 *
 * Exit 1 on a setup error.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zwp_idle_inhibit_manager_v1 *inhibit_mgr;
static struct wl_surface *surface;
static int configured;

static void global_add(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
  (void)d; (void)version;
  if (!strcmp(iface, wl_compositor_interface.name))
    compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, wl_shm_interface.name))
    shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  else if (!strcmp(iface, xdg_wm_base_interface.name))
    wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
  else if (!strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name))
    inhibit_mgr = wl_registry_bind(reg, name,
                                   &zwp_idle_inhibit_manager_v1_interface, 1);
}

static void global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }

static const struct wl_registry_listener registry_listener = {
  global_add, global_remove,
};

static void ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{ (void)d; xdg_wm_base_pong(b, serial); }

static const struct xdg_wm_base_listener wm_base_listener = { ping };

/* A 300x200 opaque shm buffer: enough to map, and wide enough that
 * enclave-mark's name tab and awake tab both fit above it. */
static struct wl_buffer *solid_buffer(void)
{
  int w = 300, h = 200, stride = w * 4, size = stride * h;
  int fd = memfd_create("live-inhibit", MFD_CLOEXEC);
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
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
  xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
  struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
  xdg_toplevel_set_app_id(tl, app_id);
  wl_surface_commit(surface);
  while (!configured && wl_display_dispatch(display) >= 0)
    ;
  wl_display_roundtrip(display);
}

int main(int argc, char **argv)
{
  int window = argc >= 3 && !strcmp(argv[1], "window");
  int early  = argc >= 3 && !strcmp(argv[1], "early");
  if (!window && !early && !(argc >= 2 && !strcmp(argv[1], "bare"))) {
    fprintf(stderr,
            "usage: live-inhibit window <app_id> | early <app_id> | bare\n");
    return 1;
  }
  struct wl_display *display = wl_display_connect(NULL);
  if (!display) {
    fprintf(stderr, "live-inhibit: cannot connect\n");
    return 1;
  }
  struct wl_registry *reg = wl_display_get_registry(display);
  wl_registry_add_listener(reg, &registry_listener, NULL);
  wl_display_roundtrip(display);
  if (!compositor || !shm || !wm_base || !inhibit_mgr) {
    fprintf(stderr, "live-inhibit: missing a global\n");
    return 1;
  }
  xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
  surface = wl_compositor_create_surface(compositor);
  if (window)
    map_window(display, argv[2]);
  struct zwp_idle_inhibitor_v1 *inh =
    zwp_idle_inhibit_manager_v1_create_inhibitor(inhibit_mgr, surface);
  wl_display_roundtrip(display);
  if (early)
    map_window(display, argv[2]);
  printf("inhibiting\n");
  fflush(stdout);

  char line[64];
  while (fgets(line, sizeof line, stdin)) {
    if (!strncmp(line, "release", 7) && inh) {
      zwp_idle_inhibitor_v1_destroy(inh);
      inh = NULL;
      wl_display_roundtrip(display);
      printf("released\n");
      fflush(stdout);
    }
  }
  wl_display_disconnect(display);
  return 0;
}
