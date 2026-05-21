# Phase 3 — DRM-KMS frontend on `drm2`

This document walks through the eight sub-phases that turn `virtio_drm`
from an fbio framebuffer driver into a real DRM-KMS driver exposing
`/dev/dri/card0`.  Each section makes one specific problem go away.

## The umbrella problem

The fbio path shipped in Phases 0-2 is enough for a console framebuffer
plus a cursor, but it's a dead-end for userland graphics — X11,
Wayland, kmscube, anything modern needs `/dev/dri/card0` and the
modesetting / GEM ioctls behind it.  Phase 3 plugs the in-base `drm2`
stack (`sys/dev/drm2/`) onto our existing virtio-gpu commands so that
interface materializes on FreeBSD/arm64, where `drm-kmod` is unavailable
(LinuxKPI is amd64-only).

---

## 3.A — drm2 dependency + driver-struct skeleton  (`0037f79`)

**Problem solved:** subsequent commits would each have to re-introduce
the drm2 include path, the driver-struct fields, and the softc plumbing
— giving us large mixed diffs that mix "infrastructure" with "feature."
Landing the skeleton first means 3.B through 3.H are each small,
mechanical, and reviewable in isolation.

**What:**
- Include `<dev/drm2/drmP.h>`, `drm_crtc.h`, `drm_crtc_helper.h`,
  `drm_edid.h`.
- Declare `static struct drm_driver virtio_drm_drm_driver` with
  `DRIVER_MODESET | DRIVER_GEM`, minimal `load`/`unload`/vblank stubs,
  empty ioctls table, name/desc/date/major/minor macros.
- Add `struct drm_device *vtgpu_drm_dev` to the softc.
- Add `pci_if.h`, `opt_drm.h`, `opt_vm.h` to the OOT module Makefile
  (drm2 pulls in `pcivar.h` + those opt_* files).
- No `MODULE_DEPEND` on `drmn`; `drm2` is expected to be statically
  linked via `device drm2` in KERNCONF.

**State at end of 3.A:** module compiles, links against drm2 symbols
when those exist in the kernel, registers nothing yet.

---

## 3.B — register a drm_device, `/dev/dri/card0` appears  (`d91440d`)

**Problem solved:** until we call `drm_get_platform_dev`, userland has
no node to open — no `xrandr`, no `Xorg`, no `kmscube` can even *try*
the driver.  This commit makes `ls /dev/dri/` return something.

**What:**
- In `attach()`, allocate a `struct drm_device` and call
  `drm_get_platform_dev(sc->vtgpu_dev, ddev, &virtio_drm_drm_driver)`.
  drm2 then creates `/dev/dri/card0` + `/dev/dri/controlD64` and fires
  our `load` callback.
- In `detach()`, call `drm_put_dev()` first (before any virtio-gpu
  host-side teardown) so userland sees the device disappear before the
  framebuffer/cursor host state is released.

**Subtle requirement caught here:** `drm_get_platform_dev`'s post-load
step calls `drm_mode_group_init_legacy_group`, which iterates
`dev->mode_config.crtc_list` — an uninitialized list head =
NULL-deref panic at boot.  So the `load` callback *must* call
`drm_mode_config_init(ddev)` before returning, even though
"mode_config" was nominally Phase 3.C work.  We do the bare-minimum
init here (open bounds, no funcs) and refine in 3.C.

**State at end of 3.B:** `ls /dev/dri/` shows `card0` + `controlD64`.
DRM core ioctls (`DRM_IOCTL_VERSION` etc.) work; mode-setting ioctls
return errors because no CRTC/connector/plane yet.

**Failure mode:** if the kernel doesn't have `device drm2`, `kldload`
succeeds (no `MODULE_DEPEND`), but our `attach()` logs
`drm_get_platform_dev failed: ENOENT (is 'device drm2' in your
KERNCONF?)` and the framebuffer-only path keeps working.

---

## 3.C — `mode_config` bounds + funcs  (`9c0fa5f`)

**Problem solved:** with placeholder `16384×16384` bounds (3.B) and a
NULL `mode_config.funcs`, every userland mode-setting validation hits a
NULL-deref the moment it tries to round-trip a real fb create.  Setting
real bounds and a real (if stubbed) funcs table means validation
returns *errors* instead of crashing.

**What:**
- Bounds derived from the host's reported scanout dimensions (cached
  by Phase 1.A: `sc->vtgpu_scanouts[i].width/height`), with `(largest
  dim) × 4` headroom for resize.  Fallback to `1920×1080` if no
  scanout is reported.
- `struct drm_mode_config_funcs` populated:
  - `fb_create`: stubbed to `-ENOSYS`.  Userland's
    `DRM_IOCTL_MODE_ADDFB2` needs a GEM buffer handle, which doesn't
    exist until 3.G.  ENOSYS gives X / kmscube a clean error rather
    than a stale fb.
  - `output_poll_changed`: no-op until fbdev emulation lands.

**State at end of 3.C:** mode-setting ioctls still error because no
CRTC, but the validation path sees plausible bounds and real funcs —
clients fail at the right *layer* rather than the framework crashing
on uninitialized fields.

---

## 3.D — single CRTC + crtc_helper funcs  (`<latest>`)

**Problem solved:** the CRTC is the modeset *sink*.  Without one,
userland can call `drmModeGetResources` but sees `count_crtcs = 0` and
nothing is drivable.  This commit makes the CRTC visible so the next
two phases (connector, plane) have something to bind to.

**What:**
- One `drm_crtc` embedded in the softc (`vtgpu_drm_crtc`).
- `drm_crtc_init` + `drm_crtc_helper_add` in `load()` after
  mode_config_init.
- crtc_helper_funcs map DRM mode-setting onto our existing virtio-gpu
  primitives:

  | DRM callback     | Implementation |
  |------------------|----------------|
  | `dpms` ON        | `vtgpu_set_scanout(fb_resource, w, h)` |
  | `dpms` OFF       | `SET_SCANOUT(resource_id=0)` to blank |
  | `mode_set`       | `set_scanout` + `transfer_to_host_2d` + `flush` |
  | `mode_set_base`  | re-run `mode_set` with same mode |
  | `prepare`/`commit` | dpms-off / dpms-on brackets |
  | `mode_fixup`     | accept any timing unchanged |

- `set_config = drm_crtc_helper_set_config`, so userland's
  `DRM_IOCTL_MODE_SETCRTC` goes through the standard helper that
  drives our `mode_set` + `mode_set_base`.

**State at end of 3.D:** CRTC visible in mode-setting ioctls but no
modes to attach yet (no connector); X11 modesetting driver will still
refuse to attach.

**Limitation to know about:** the CRTC currently drives **only the
attach-time fb resource** (the vt console fb).  Userland can't yet
bind a new `drm_framebuffer` here — that's a 3.G/H story.  So even a
perfectly-issued `drmModeSetCrtc(...)` from userland will display
console contents at the requested geometry.

---

## 3.E — connector + EDID-driven modes  (pending)

**Problem solved:** without a connector, userland has no way to learn
*what modes the device supports*.  3.D's CRTC is a sink with no fuel.
This commit feeds the EDID we already cached in Phase 1.B into
`drm_add_edid_modes()` so `xrandr` (and X server probing) sees a real
mode list — 640×480, 800×600, 1024×768, 1920×1080, etc., whatever
QEMU's EDID generator declared.

**What:**
- One `drm_connector` per enabled virtio-gpu scanout (start with one)
  of type `DRM_MODE_CONNECTOR_VIRTUAL`.
- `drm_connector_init` + `drm_connector_helper_add`.
- `get_modes()` callback feeds `sc->vtgpu_scanouts[0].edid` into
  `drm_add_edid_modes()`.
- `mode_valid()` clips to `mode_config.max_*` and to our framebuffer
  dimensions.
- `detect()` returns `connector_status_connected` (virtio-gpu scanouts
  don't disappear).

**State after 3.E:**  X11 modesetting driver can
`drmModeGetConnector`, fetch modes, and call `drmModeSetCrtc` — and
our CRTC will honor it (within the fb-from-attach limitation above).

---

## 3.F — primary plane  *(NOT APPLICABLE — drm2)*

**Problem solved:** *(nothing — this sub-phase doesn't exist in the
drm2 model.)*

**Why:**  Universal planes — and `DRM_PLANE_TYPE_PRIMARY` — were
introduced in the atomic-modesetting branch of mainline DRM, *after*
the drm2 fork that ships in FreeBSD base.  In drm2 the primary plane
is **implicit in the CRTC**: the CRTC's `mode_set` + `mode_set_base`
callbacks (which we wired in 3.D) own the primary scanout source
directly.  `drm_plane_init` in drm2 is reserved for *overlay* planes,
which we don't need.

This means 3.F is a **no-op** for our driver — there's no commit and
no code, just this note.  Phase 3.G (dumb buffers) is what actually
delivers "userland can bind its own framebuffer to the CRTC."

---

## 3.G — dumb buffer alloc  (pending)

**Problem solved:** "dumb" buffers are how userland creates a
displayable framebuffer without needing a full GEM allocator.  Until
this lands, userland *cannot create a drm_framebuffer at all* —
`drmModeAddFB2` and `drmModeMapDumb` both fail, which means no Xorg,
no kmscube, no Wayland.  This is the single biggest user-facing
unblock in Phase 3.

**What:**
- `drm_driver.dumb_create`: handle `DRM_IOCTL_MODE_CREATE_DUMB` by
  `contigmalloc`ing a `width × height × bpp/8` buffer, wrapping it in
  a GEM object, registering a handle.
- `drm_driver.dumb_map_offset`: produce an mmap offset so userland can
  `mmap` the buffer.
- `drm_driver.dumb_destroy`: free the GEM object + the underlying
  contig pages.
- Replace `fb_create`'s `-ENOSYS` stub with a real implementation that
  wraps the passed GEM handle in a `drm_framebuffer`.
- New virtio-gpu commands per dumb buffer: `RESOURCE_CREATE_2D` +
  `ATTACH_BACKING` + a per-buffer `vtgpu_have_resource`-style tracking
  in the GEM object.

**State after 3.G:** `kmscube`, `weston-simple-egl`, and the X11
modesetting driver can `drmModeAddFB2` + `drmModeSetCrtc` against
their own buffers and see output on screen.

---

## 3.H — vt(4) handover  *(shipped)*

**Problem solved:** vt(4) and the DRM userland session both want
ownership of the framebuffer.  Without explicit handover, they fight:
the console keeps overwriting X, or X starves the console, or both
crash.  This commit makes the two coexist cleanly.

**What:**
- `drm_driver.firstopen`: on the first open of `/dev/dri/card0`, call
  `vt_deallocate(&vtgpu_fb_driver, &sc->vtgpu_fb_info)` and clear
  `vtgpu_have_fb_info` so vt(4) stops drawing into our framebuffer
  pages.
- `drm_driver.lastclose`: on the last DRM fd close, re-`vt_allocate()`
  if `vtgpu_have_fb_info` is false but `fb_vbase` is still valid.
  Console comes back.

**State after 3.H:** full DRM-KMS pipeline.  Userland sessions grab
the display, modeset to EDID-derived resolutions, hand back to vt(4)
on exit.  SLiM / X11 / Wayland are now viable on FreeBSD/arm64 VMs.

**Note on re-allocate priority warning:**  vt(4) prints
`Driver priority 110 too low. Current 110` when we re-allocate the
same vt driver.  It's a vt internal warning when the new candidate's
priority equals the current; benign for our re-allocate case because
we're the same driver re-entering, not a different one trying to
preempt.

---

## Open known issues (cross-cutting)

These don't belong to a single sub-phase but are worth tracking:

- **Dynamic resize via `VIRTIO_GPU_EVENT_DISPLAY`** relies on the
  3.F-H plane/fb path.  Until then, resizing the QEMU window mid-boot
  produces ghosted output (our fb stays at attach-time size while the
  host scanout grows).  Workaround: fix QEMU window size via
  `-device virtio-gpu-pci,xres=...,yres=...`.
- **In-base `virtio_gpu` collision** mitigated by `BUS_PROBE_VENDOR`
  priority (see 3.A driver-struct).  Works for runtime kldload and
  for preload.  `nodevice virtio_gpu` in KERNCONF avoids the race
  entirely.
- **Cursor coordinates from real input** still go through the sysctl
  test surface (Phase 2.D).  3.E's connector + hotplug machinery makes
  hooking a USB-tablet event source natural, but it's not on the
  critical path for X11 — X handles cursor on the userland side once
  the device is open.
