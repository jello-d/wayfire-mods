# wayfire-mods

A complete from-source Wayland compositor stack, built into a self-contained
prefix: [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) (clean
upstream) + [Wayfire](https://github.com/WayfireWM/wayfire) (with a few patches)
+ wcm / wayfire-plugins-extra / wf-info (clean) + the filters and pixdecor
decorators (with patches) + a set of custom Wayfire plugins + a patched
[xdg-desktop-portal-wlr](https://github.com/emersion/xdg-desktop-portal-wlr)
backend (window-only sharing for a sandboxed client; see
`patches/xdpw-window-only.md`).

It is **not a fork**. Each upstream component is fetched at a pinned ref
(`refs`) and assembled onto a pristine checkout: our patches (`patches/`) are
`git apply`-ed, our plugins (`plugins/`) build against the installed Wayfire.
Every addition stays a discrete unit, and the base is refreshed by bumping a ref
rather than by rebasing a divergent tree.

## Layout

- `refs`: the upstream URLs + pinned refs. wlroots and Wayfire are ABI-coupled
  and pinned together here; every satellite links that ABI, so a wlroots or
  Wayfire bump rebuilds them all.
- `patches/`: patches for Wayfire (ipc-guard, startup-image, scale-clamp,
  security-context-allowlist, output-commit-backoff,
  ipc-name-dropped-client), filters
  (frame-margins), pixdecor
  (titlebar-padding), and xdg-desktop-portal-wlr (window-only). Only that
  backend's BINARY is installed (`/opt/wayfire/libexec`); the distro package
  keeps its unit and D-Bus files, and `setup.sh install` writes a drop-in,
  `/etc/systemd/user/xdg-desktop-portal-wlr.service.d/50-wayfire-mods.conf`,
  pointing that unit at it. wlroots, wcm, plugins-extra, and wf-info are clean
  upstream (no patch).
- `plugins/`: the custom Wayfire plugins (edge-flipping, edge-warp,
  workspace-wallpaper, wsync, directional-focus, atlas, ...), a
  self-contained meson project.
- `libexec/build-wayfire`: the whole build recipe (fetch, patch, build in
  dependency order, rpath, incremental stamps).

## Use

```sh
sudo ./setup.sh install   # build + install into /opt/wayfire (incremental)
./setup.sh verify         # throwaway build, no sudo, then run it headless
./setup.sh check          # installed + self-contained + components? [OK]/[FAIL]
./setup.sh uninstall      # remove the prefix + stamps
./setup.sh version        # package version + wlroots/Wayfire pins
```

`install` is incremental (each component stamp-gated) and sudo-safe (it skips
rather than hang if it cannot get the prefix). Every installed ELF carries a
relative `<prefix>/lib/<arch>` rpath, so the prefix is self-contained with no
ld.so.conf entry. To ride new upstream: edit `refs`, `./setup.sh verify`, and
commit the bump if it passes.

`patches/` and `plugins/` are Wayfire/wlroots C++; follow upstream style there.

## Scope

This builds the compositor **platform** only. The greeter/session stack (greetd,
the login sway config, swaylock, ReGreet) and all runtime configuration are the
consumer's, not part of this package.

## License

Apache-2.0. Upstream components are fetched at build time under their own
licenses, not vendored here.
