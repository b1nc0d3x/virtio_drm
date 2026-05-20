# virtio_drm — FreeBSD-native virtio_gpu DRM-KMS driver (arm64)

Native FreeBSD DRM-KMS driver for the paravirtual `virtio-gpu`
device, targeting arm64 — the architecture where the LinuxKPI-
based `drm-kmod` stack is unavailable.

Forked from `sys/dev/virtio/gpu/virtio_gpu.c` in FreeBSD base
(Bryan Venteicher 2013, Arm Ltd 2023) under the same BSD-2-Clause
license.  Project scope is to extend that driver from the
fbio-only framebuffer it provides today to a full DRM-KMS
frontend on top of the in-base `drm2` stack — restoring modeset,
multi-scanout, EDID, cursor, and (eventually) blob resources to
arm64 FreeBSD VMs.

## Build

Cross-build from a FreeBSD/amd64 host with `/usr/src` present:

```sh
cd /usr/src
sudo make buildenv TARGET_ARCH=aarch64 \
     BUILDENV_SHELL='sh -c "cd /path/to/virtio_drm/src && make"'
```

Native build on a FreeBSD/arm64 target:

```sh
cd /path/to/virtio_drm/src
make
```

Output: `virtio_drm.ko` in `src/`.

## Status

  - **Phase 0 (today):**  fork compiled cleanly as `virtio_drm.ko`
    for arm64.  Behavior is identical to the in-base
    `virtio_gpu` — fbio framebuffer, fixed resolution.
  - **Phase 1:**  add missing `VIRTIO_GPU_CMD_*` paths
    (RESOURCE_UNREF, DETACH_BACKING, GET_EDID, hotplug events,
    multi-scanout).  Result: dynamic resize + EDID-driven modes
    + multi-monitor.
  - **Phase 2:**  cursor (UPDATE_CURSOR, MOVE_CURSOR).
  - **Phase 3:**  graft DRM-KMS frontend on `drm2` —
    `/dev/dri/card0`, X11 modesetting driver.

## License

BSD-2-Clause.  See header in each source file for the chain of
copyright (Venteicher 2013, Arm Ltd 2023, Crenshaw 2026).
