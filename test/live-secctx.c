/*
 * live-secctx <socket-path> <instance-id>: register a security-context-v1
 * listener on the compositor named by WAYLAND_DISPLAY, the way a sandbox
 * engine (Flatpak, or severance for an enclave) does, then wait to be
 * killed. Every client that connects to <socket-path> is in that context.
 * Prints "ready" once the context is committed. For test/live.t.
 *
 * The close fd is the read end of a pipe whose write end this process
 * holds, so the context is torn down when this process exits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include "security-context-v1-client-protocol.h"

static struct wp_security_context_manager_v1 *manager;

static void global_add(void *data, struct wl_registry *registry,
                       uint32_t name, const char *interface, uint32_t version)
{
  (void)data; (void)version;
  if (strcmp(interface, wp_security_context_manager_v1_interface.name) == 0)
    manager = wl_registry_bind(registry, name,
                               &wp_security_context_manager_v1_interface, 1);
}

static void global_remove(void *data, struct wl_registry *registry,
                          uint32_t name)
{
  (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
  global_add, global_remove,
};

static int listen_on(const char *path)
{
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 || strlen(path) >= sizeof(addr.sun_path))
    return -1;
  strcpy(addr.sun_path, path);
  unlink(path);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0
      || listen(fd, 16) < 0)
    return -1;
  return fd;
}

int main(int argc, char **argv)
{
  if (argc != 3) {
    fprintf(stderr, "usage: live-secctx <socket-path> <instance-id>\n");
    return 2;
  }
  struct wl_display *display = wl_display_connect(NULL);
  if (!display) {
    fprintf(stderr, "live-secctx: cannot connect to the compositor\n");
    return 1;
  }
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  if (!manager) {
    fprintf(stderr, "live-secctx: no wp_security_context_manager_v1\n");
    return 1;
  }
  int listen_fd = listen_on(argv[1]);
  int close_fds[2];
  if (listen_fd < 0 || pipe(close_fds) < 0) {
    perror("live-secctx");
    return 1;
  }
  struct wp_security_context_v1 *ctx =
    wp_security_context_manager_v1_create_listener(manager, listen_fd,
                                                   close_fds[0]);
  wp_security_context_v1_set_sandbox_engine(ctx, "wayfire-mods-test");
  wp_security_context_v1_set_app_id(ctx, "wfm-test");
  wp_security_context_v1_set_instance_id(ctx, argv[2]);
  wp_security_context_v1_commit(ctx);
  close(listen_fd);
  close(close_fds[0]);
  if (wl_display_roundtrip(display) < 0) {
    fprintf(stderr, "live-secctx: the compositor rejected the context\n");
    return 1;
  }
  printf("ready\n");
  fflush(stdout);
  while (wl_display_dispatch(display) != -1)
    ;
  return 0;
}
