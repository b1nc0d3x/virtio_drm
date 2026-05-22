# How a pixel gets from your app to the screen

This is the end-to-end path through `virtio_drm` for a single pixel that an
application (xterm, kmscube, Wayland compositor) draws.  It covers the userland
ioctls, drm2 core dispatch, our driver, the virtio-gpu protocol, QEMU host-side
handling, and finally the GTK/VNC window where you see it.

---

## 0. The cast

```
 ┌─────────────────────────────────────────────────────────────────────┐
 │  GUEST  (FreeBSD VM)                                                │
 │                                                                     │
 │   user-space        ┌────────────────┐                              │
 │   processes ──────→ │  libdrm        │                              │
 │   (Xorg,            │  ioctl()       │                              │
 │   xterm, …)         └───────┬────────┘                              │
 │                             │                                       │
 │   ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─│─ syscall boundary ─ ─ ─ ─ ─ ─ ─ ─ ─ ─│
 │                             ▼                                       │
 │   kernel       ┌─────────────────────────┐                          │
 │                │  drm2 core              │                          │
 │                │  /usr/src/sys/dev/drm2/ │                          │
 │                └────────────┬────────────┘                          │
 │                             │ callbacks                             │
 │                             ▼                                       │
 │                ┌─────────────────────────┐                          │
 │                │  virtio_drm (us)        │                          │
 │                │  - drm_driver           │                          │
 │                │  - crtc / connector /   │                          │
 │                │    framebuffer / gem    │                          │
 │                └────────────┬────────────┘                          │
 │                             │ virtio-gpu protocol                   │
 │                             ▼                                       │
 │                ┌─────────────────────────┐                          │
 │                │  virtio_pci  (built-in) │                          │
 │                │  - control vq, cursor vq│                          │
 │                └────────────┬────────────┘                          │
 │                             │ virtqueue descriptors                 │
 └─────────────────────────────│───────────────────────────────────────┘
                               │
                               ▼   (PCI MMIO + shared RAM)
 ┌─────────────────────────────────────────────────────────────────────┐
 │  HOST  (QEMU on b3astm0d3x86)                                       │
 │                                                                     │
 │            ┌──────────────────────────┐                             │
 │            │  hw/display/virtio-gpu*  │   reads guest RAM directly  │
 │            │  (QEMU's virtio-gpu      │   on TRANSFER_TO_HOST_2D    │
 │            │   device backend)        │                             │
 │            └────────────┬─────────────┘                             │
 │                         │ host-side image surface                   │
 │                         ▼                                           │
 │            ┌──────────────────────────┐                             │
 │            │  QEMU UI subsystem       │                             │
 │            │  (GTK / VNC / SDL)       │                             │
 │            └────────────┬─────────────┘                             │
 └─────────────────────────│───────────────────────────────────────────┘
                           │
                           ▼
                  the screen you see
```

---

## 1. Buffer creation

Userland wants a 1024×768 framebuffer to draw into.

```
 user      libdrm                       drm core           virtio_drm
 ──────    ──────────                   ─────────────      ──────────────────
 [X.org]
   │
   │ drmModeCreateDumb(w=1024, h=768, bpp=32)
   └──→ ioctl(/dev/dri/card0, DRM_IOCTL_MODE_CREATE_DUMB)
            │
            └──→ drm_ioctl()
                     │ table lookup by ioctl number
                     └──→ dispatches to driver.dumb_create
                              │
                              └──→ virtio_drm_dumb_create()
                                       │
                                       │ contigmalloc(W*H*4)
                                       │ allocate resource_id  (e.g. 7)
                                       │ drm_gem_object_init()
                                       │
                                       │  ┌─────────────────────────┐
                                       │  │ struct vtgpu_gem_bo {   │
                                       │  │   gem_obj               │
                                       │  │   vbase (kernel VA)     │
                                       │  │   pbase (phys addr)     │
                                       │  │   resource_id = 7       │
                                       │  │ }                       │
                                       │  └─────────────────────────┘
                                       │
                                       │  virtio_drm_bo_create_host_resource:
                                       │    ─── ctrl vq ───→  RESOURCE_CREATE_2D
                                       │                       (id=7, fmt, w, h)
                                       │    ─── ctrl vq ───→  RESOURCE_ATTACH_BACKING
                                       │                       (id=7, paddr=pbase,
                                       │                        len=W*H*4)
                                       │
                                       │ drm_gem_handle_create() ──→ handle=42
                                       │ TAILQ_INSERT to sc->vtgpu_bos
                                       │
                              return handle=42, pitch=4096, size=W*H*4
```

After this, the host has:

```
 host-side resource table
   id 1 : (our attach-time primary fb, the vt console)
   id 2 : (our 64x64 BGRA cursor)
   id 7 : (1024x768 BGRA, backed by guest pages at paddr=pbase)
```

The guest has a `struct vtgpu_gem_bo` registered in `sc->vtgpu_bos` and a GEM
handle that user-space refers to as "buffer 42".

---

## 2. mmap — getting the buffer into the X server's address space

```
 [X.org]
   │
   │ drmModeMapDumb(handle=42)  ──→  ioctl(DRM_IOCTL_MODE_MAP_DUMB)
   │   returns mmap_offset
   │
   │ mmap(NULL, size, RW, SHARED, /dev/dri/card0, mmap_offset)
   └──→ FreeBSD VM:
           drm2's cdev mmap entry
              │
              └──→ drm_gem_mmap_single()
                       │
                       │ cdev_pager_allocate(handle=gem_obj,
                       │                     ops=virtio_drm_gem_pager_ops,
                       │                     size, ...)
                       │
                       │ returns a fresh vm_object whose page-fault
                       │ handler is virtio_drm_gem_pager_fault.
                       │
                       └──→ vm_mmap_object()
                                │
                                └── installs the vm_object in X's
                                    address space as a VM map entry
```

When X writes the first pixel:

```
 X writes *(uint32_t*)ptr = 0xFFFF0000
   │
   │ MMU has no PTE for that page yet → page fault
   ▼
 trap → vm_fault_trap → vm_fault_lookup → vm_pager_get_pages
   │
   └──→ virtio_drm_gem_pager_fault(vm_obj, offset, prot, &mres)
            │
            │ paddr  = bo->pbase + offset    ; same physical page our
            │ page   = PHYS_TO_VM_PAGE(paddr); contigmalloc returned
            │ vm_page_insert(page, vm_obj, ...)
            │ *mres = page
            │ valid = ALL
            └──→ VM_PAGER_OK
   │
   ▼ MMU now has a PTE → write completes into that physical page
   ▼ ... but the value is in CPU cache; physical RAM may still be 0
```

---

## 3. Telling DRM the buffer is a *framebuffer*

```
 drmModeAddFB2(handle=42, w, h, pitches[0]=4096, ...)
   │
   └──→ ioctl(DRM_IOCTL_MODE_ADDFB2)
            │
            └──→ drm_mode_addfb2 → driver.fb_create
                     │
                     └──→ virtio_drm_fb_create()
                              │
                              │ struct virtio_drm_framebuffer fb = {
                              │   .base  = drm_framebuffer{w, h, pitch, …},
                              │   .bo    = our bo for handle=42,
                              │ }
                              │ drm_framebuffer_init(fb, virtio_drm_fb_funcs)
                              │
                          return fb_id=10
```

Now userland has a `fb_id` it can attach to a CRTC.

---

## 4. Modeset — pointing the scanout at our buffer

```
 drmModeSetCrtc(crtc, fb_id=10, x=0, y=0, modes[0])
   │
   └──→ ioctl(DRM_IOCTL_MODE_SETCRTC)
            │
            └──→ drm_crtc_helper_set_config
                     │
                     ├── prepare → driver.crtc_helper.prepare (dpms OFF)
                     ├── mode_set → virtio_drm_crtc_mode_set
                     │      │
                     │      │  fb  = crtc->fb (our virtio_drm_framebuffer)
                     │      │  rid = fb->bo->resource_id   (= 7)
                     │      │
                     │      │  ── ctrl vq ──→ SET_SCANOUT(scanout=0, rid=7, w, h)
                     │      │  ── ctrl vq ──→ TRANSFER_TO_HOST_2D
                     │      │  ── ctrl vq ──→ RESOURCE_FLUSH
                     │      │
                     │      │  sc->vtgpu_drm_active_resource_id = 7
                     │      │  sc->vtgpu_drm_active_bo          = fb->bo
                     │      │  virtio_drm_flush_arm()
                     │      │
                     ├── commit → driver.crtc_helper.commit  (dpms ON)
                     └── done
```

After modeset the host knows: *"display 0 = read pixels from resource 7"*.

---

## 5. Pixels reach the display — three pump paths

Whichever path fires, the steps from "write in guest memory" → "pixels on the
host display" are the same three host-side commands:

```
                              ┌─────────────────────────────────┐
                              │  Userland wrote into bo pages   │
                              │  (but writes sit in CPU cache!) │
                              └────────────────┬────────────────┘
                                               │
              ┌────────────────────────────────┼─────────────────────────────────┐
              │                                │                                 │
              ▼  PATH A                        ▼  PATH B                         ▼  PATH C
  ┌─────────────────────┐         ┌────────────────────────┐         ┌────────────────────────┐
  │ drmModeDirtyFB ioctl│         │ periodic flush callout │         │ vt(4) console blit     │
  │ (DRI3 / shadowfb)   │         │ vtgpu_drm_flush_tick   │         │ (kernel text rendering)│
  │ → fb_funcs.dirty()  │         │ (1 Hz, our safety net) │         │ → vd_bitblt_* helpers  │
  └──────────┬──────────┘         └───────────┬────────────┘         └────────────┬───────────┘
             │                                │                                   │
             └────────────────┬───────────────┴─────────────────┬─────────────────┘
                              ▼                                 ▼
                  ┌───────────────────────────┐    cpu_dcache_wb_range(bo->vbase, size)
                  │ Cache writeback           │    (essential: CPU cache → RAM)
                  └────────────┬──────────────┘
                               ▼
                ── ctrl vq ──→ TRANSFER_TO_HOST_2D
                               (rid, x, y, w, h, offset)
                                          ↓
                ── ctrl vq ──→ RESOURCE_FLUSH
                               (rid, x, y, w, h)
                                          ↓
                ─── virtqueue_notify ───→  vmexit into QEMU
```

---

## 6. Host-side (QEMU)

```
 hw/display/virtio-gpu-base.c
   │
   ▼
 virtio_gpu_process_cmd  ──→ virtio_gpu_transfer_to_host_2d
   │                            │
   │                            │  memcpy from guest physical memory
   │                            │  (the pages we ATTACH_BACKING'd)
   │                            │  into the host-side QEMU image
   │                            │  associated with resource 7
   │                            ▼
   │                       host image now has fresh pixels
   │
   └──→ virtio_gpu_resource_flush
            │
            │ dpy_gfx_update(con, x, y, w, h)
            ▼
       QEMU UI thread:
        - GTK: invalidate widget rect, GTK paints from host image
        - VNC: enqueue framebuffer update message to clients
        - SDL: blit to SDL_Surface, SDL_RenderPresent
            │
            ▼
       Pixels visible in the QEMU window.
```

---

## 7. The cursor takes a separate path

```
 sysctl dev.vgpu.0.cursor_test_pattern=1
   │
   ▼
 paint magenta into bo->vbase   (cursor resource id = 2, 64x64 BGRA)
   │
 ── ctrl vq ──→ TRANSFER_TO_HOST_2D (rid=2)
   │
 ── cursor vq ──→ UPDATE_CURSOR (scanout=0, x, y, hot_x, hot_y, rid=2)

 sysctl dev.vgpu.0.cursor_xy="500 400"
   │
 ── cursor vq ──→ MOVE_CURSOR (scanout=0, x=500, y=400)
                            (no bitmap transfer — fast path)
```

The cursor virtqueue (vq #1) is independent of the control vq so mouse-rate
position updates don't get stuck behind a slow 2D upload.

---

## 8. Open gap (Phase 4, in progress)

In the diagram, **PATH A (drmModeDirtyFB)** only fires when userland uses DRI3
to share buffer fds.  In our current implementation:

- Our `fb_funcs.dirty` is wired and ready
- But `xf86-video-modesetting` without DRI3 never calls `DRM_IOCTL_MODE_DIRTYFB`
- And drm2's PRIME ioctl bodies are `#ifdef FREEBSD_NOTYET` (not compiled in),
  so DRI3 isn't available — userland can't get a buffer fd to share

So today **PATH B (periodic callout)** is the only userland-write delivery
mechanism, which is enough to demonstrate the pipeline (we proved it with the
red/blue/green sysctl fills) but X never *uses* the dumb buffer for rendering
in this driverless-DRI3 configuration.

Phase 4 fills in PRIME so DRI3 lights up and X's `dirty` calls reach us
through PATH A — the proper, surgical, dirty-rect-driven path.
