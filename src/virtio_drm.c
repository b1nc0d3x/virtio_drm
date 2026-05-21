/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2013, Bryan Venteicher <bryanv@FreeBSD.org>
 * All rights reserved.
 * Copyright (c) 2023, Arm Ltd
 * Copyright (c) 2026, Kyle Crenshaw <b1nc0d3x@gmail.com>
 *
 * Forked from sys/dev/virtio/gpu/virtio_gpu.c to extend the
 * remaining VIRTIO_GPU_CMD_* paths, add multi-scanout / EDID /
 * hotplug, and graft a DRM-KMS frontend on top of the in-base
 * drm2 stack.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Driver for the paravirtual VirtIO GPU device.
 *
 * ===========================================================================
 * Why a virtual GPU driver?
 * ===========================================================================
 *
 * FreeBSD/amd64 gets a modern DRM-KMS graphics stack through `drm-kmod`, a
 * port of the Linux DRM drivers built on top of LinuxKPI (a Linux-kernel-API
 * compatibility shim).  LinuxKPI has never been ported to aarch64.  As a
 * result, every FreeBSD-arm64 VM and host today is stuck on `efifb`:
 *
 *   - a static framebuffer at whatever resolution UEFI happened to set,
 *   - no modeset, no DPMS, no GPU acceleration,
 *   - no `/dev/dri/card0`, so no X11 modesetting driver, no Wayland,
 *   - no dynamic resize when the QEMU/UTM window changes,
 *   - no second-monitor support.
 *
 * `virtio-gpu` is the paravirtual GPU defined by the VirtIO spec.  It is the
 * display device exposed by every modern hypervisor that matters for arm64:
 * QEMU/KVM, Apple's Virtualization.framework (UTM, vagrant-apple-vz), Cloud
 * Hypervisor, Firecracker, and the arm64 paths of VMware and Xen.
 *
 * A real DRM-KMS driver for virtio-gpu therefore single-handedly closes the
 * arm64 graphics gap for VMs — which is the dominant arm64-FreeBSD use case
 * today (UTM on Apple Silicon, AWS Graviton consoles, qemu-system-aarch64
 * dev VMs).  It also validates the methodology for non-LinuxKPI DRM drivers
 * on FreeBSD/arm64, which is the same template needed for native hardware
 * like Apple AGX, Mali, and the Rockchip Cadence DP.
 *
 * The in-base `sys/dev/virtio/gpu/virtio_gpu.c` (Bryan Venteicher 2013,
 * Arm Ltd 2023) implements ~6 of the ~25 VirtIO GPU commands and exposes
 * a fixed-resolution fbio framebuffer.  This driver picks up where that
 * scaffold left off:
 *
 *   Phase 1 (this commit train): RESOURCE_UNREF / DETACH_BACKING lifecycle,
 *           runtime GET_EDID, VIRTIO_GPU_EVENT_DISPLAY hotplug listener,
 *           multi-scanout enumeration.  Still an fbio framebuffer but
 *           now reflects what the host is actually advertising.
 *   Phase 2:  cursor (UPDATE_CURSOR, MOVE_CURSOR) on the cursor vq.
 *   Phase 3:  DRM-KMS frontend on the in-base drm2 stack — single CRTC,
 *           connector, plane, dumb-buffer alloc → /dev/dri/card0.
 *   Phase 4+: blob resources, render node, eventually VirGL/Venus.
 * ===========================================================================
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/fbio.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sglist.h>
#include <sys/sysctl.h>

#include <machine/atomic.h>
#include <machine/bus.h>
#include <machine/resource.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <dev/virtio/virtio.h>
#include <dev/virtio/virtqueue.h>
#include <dev/virtio/gpu/virtio_gpu.h>

#include <dev/vt/vt.h>
#include <dev/vt/hw/fb/vt_fb.h>
#include <dev/vt/colors/vt_termcolors.h>

#include <dev/drm2/drmP.h>
#include <dev/drm2/drm_crtc.h>
#include <dev/drm2/drm_crtc_helper.h>
#include <dev/drm2/drm_edid.h>

#include "fb_if.h"
#include "virtio_if.h"

#define	VIRTIO_DRM_DRIVER_NAME	"virtio_drm"
#define	VIRTIO_DRM_DRIVER_DESC	"FreeBSD VirtIO GPU DRM-KMS (arm64)"
#define	VIRTIO_DRM_DRIVER_DATE	"20260521"
#define	VIRTIO_DRM_DRIVER_MAJOR	0
#define	VIRTIO_DRM_DRIVER_MINOR	1

#define VTGPU_FEATURES	(1ULL << VIRTIO_GPU_F_EDID)

/* The guest can allocate resource IDs, we only need one */
#define	VTGPU_RESOURCE_ID	1

/*
 * Per-scanout state.  Phase 1 enumerates all scanouts reported
 * by VIRTIO_GPU_CMD_GET_DISPLAY_INFO so multi-monitor configs
 * are visible at boot; the framebuffer itself still drives only
 * the primary (vd_scanout) scanout until DRM-KMS lands in Phase
 * 3.
 */
struct vtgpu_scanout {
	uint32_t	x;
	uint32_t	y;
	uint32_t	width;
	uint32_t	height;
	uint32_t	flags;
	bool		enabled;
	uint32_t	edid_size;	/* 0 if no EDID for this scanout */
	uint8_t		edid[1024];
};

struct vtgpu_softc {
	/* Must be first so we can cast from info -> softc */
	struct fb_info 		 vtgpu_fb_info;
	struct virtio_gpu_config vtgpu_gpucfg;

	device_t		 vtgpu_dev;
	uint64_t		 vtgpu_features;

	struct virtqueue	*vtgpu_ctrl_vq;
	struct virtqueue	*vtgpu_cursor_vq;	/* Phase 2: vq #1 */

	uint64_t		 vtgpu_next_fence;

	bool			 vtgpu_have_fb_info;

	struct vtgpu_scanout	 vtgpu_scanouts[VIRTIO_GPU_MAX_SCANOUTS];
	uint32_t		 vtgpu_primary_scanout;

	/* Resource lifecycle bookkeeping for clean detach(). */
	bool			 vtgpu_have_resource;
	bool			 vtgpu_have_backing;

	/* Phase 2: hardware cursor resource. */
	vm_offset_t		 vtgpu_cursor_vbase;
	bus_addr_t		 vtgpu_cursor_pbase;
	bool			 vtgpu_have_cursor_resource;
	bool			 vtgpu_have_cursor_backing;
	uint32_t		 vtgpu_cursor_x;
	uint32_t		 vtgpu_cursor_y;

	/* Phase 3: DRM-KMS frontend on the in-base drm2 stack. */
	struct drm_device	*vtgpu_drm_dev;
	struct drm_crtc		 vtgpu_drm_crtc;	/* one CRTC per device */
	struct drm_encoder	 vtgpu_drm_encoder;
	struct drm_connector	 vtgpu_drm_connector;

	/* Phase 3.G: dumb-buffer resource_id allocator (starts past 1
	 * (primary fb) and 2 (cursor)). */
	uint32_t		 vtgpu_next_resource_id;
};

#define	VTGPU_CURSOR_RESOURCE_ID	2
#define	VTGPU_CURSOR_WIDTH		64
#define	VTGPU_CURSOR_HEIGHT		64
#define	VTGPU_CURSOR_BPP		4
#define	VTGPU_CURSOR_SIZE		(VTGPU_CURSOR_WIDTH * \
					 VTGPU_CURSOR_HEIGHT * \
					 VTGPU_CURSOR_BPP)

static int	vtgpu_modevent(module_t, int, void *);

static int	vtgpu_probe(device_t);
static int	vtgpu_attach(device_t);
static int	vtgpu_detach(device_t);
static int	vtgpu_config_change(device_t);

static int	vtgpu_negotiate_features(struct vtgpu_softc *);
static int	vtgpu_setup_features(struct vtgpu_softc *);
static void	vtgpu_read_config(struct vtgpu_softc *,
		    struct virtio_gpu_config *);
static int	vtgpu_alloc_virtqueue(struct vtgpu_softc *);
static int	vtgpu_req_resp(struct vtgpu_softc *, void *, size_t,
		    void *, size_t);
static int	vtgpu_get_display_info(struct vtgpu_softc *);
static int	vtgpu_get_edid(struct vtgpu_softc *, uint32_t);
static int	vtgpu_create_2d(struct vtgpu_softc *);
static int	vtgpu_attach_backing(struct vtgpu_softc *);
static int	vtgpu_detach_backing(struct vtgpu_softc *);
static int	vtgpu_resource_unref(struct vtgpu_softc *);
static int	vtgpu_set_scanout(struct vtgpu_softc *, uint32_t, uint32_t,
		    uint32_t, uint32_t);
static int	vtgpu_transfer_to_host_2d(struct vtgpu_softc *, uint32_t,
		    uint32_t, uint32_t, uint32_t);
static int	vtgpu_resource_flush(struct vtgpu_softc *, uint32_t, uint32_t,
		    uint32_t, uint32_t);

/* Phase 2: hardware cursor. */
static int	vtgpu_cursor_create_resource(struct vtgpu_softc *);
static int	vtgpu_cursor_attach_backing(struct vtgpu_softc *);
static int	vtgpu_cursor_detach_backing(struct vtgpu_softc *);
static int	vtgpu_cursor_resource_unref(struct vtgpu_softc *);
static int	vtgpu_cursor_transfer(struct vtgpu_softc *);
static int	vtgpu_update_cursor(struct vtgpu_softc *, uint32_t, uint32_t,
		    uint32_t, uint32_t, uint32_t);
static int	vtgpu_move_cursor(struct vtgpu_softc *, uint32_t, uint32_t,
		    uint32_t);
static void	vtgpu_sysctl_setup(struct vtgpu_softc *);

static vd_blank_t		vtgpu_fb_blank;
static vd_bitblt_text_t		vtgpu_fb_bitblt_text;
static vd_bitblt_bmp_t		vtgpu_fb_bitblt_bitmap;
static vd_drawrect_t		vtgpu_fb_drawrect;
static vd_setpixel_t		vtgpu_fb_setpixel;
static vd_bitblt_argb_t		vtgpu_fb_bitblt_argb;

static struct vt_driver vtgpu_fb_driver = {
	.vd_name = "virtio_drm",
	.vd_init = vt_fb_init,
	.vd_fini = vt_fb_fini,
	.vd_blank = vtgpu_fb_blank,
	.vd_bitblt_text = vtgpu_fb_bitblt_text,
	.vd_invalidate_text = vt_fb_invalidate_text,
	.vd_bitblt_bmp = vtgpu_fb_bitblt_bitmap,
	.vd_bitblt_argb = vtgpu_fb_bitblt_argb,
	.vd_drawrect = vtgpu_fb_drawrect,
	.vd_setpixel = vtgpu_fb_setpixel,
	.vd_postswitch = vt_fb_postswitch,
	.vd_priority = VD_PRIORITY_GENERIC+10,
	.vd_fb_ioctl = vt_fb_ioctl,
	.vd_fb_mmap = NULL,	/* No mmap as we need to signal the host */
	.vd_suspend = vt_fb_suspend,
	.vd_resume = vt_fb_resume,
};

VT_DRIVER_DECLARE(vt_virtio_drm, vtgpu_fb_driver);

static void
vtgpu_fb_blank(struct vt_device *vd, term_color_t color)
{
	struct vtgpu_softc *sc;
	struct fb_info *info;

	info = vd->vd_softc;
	sc = (struct vtgpu_softc *)info;

	vt_fb_blank(vd, color);

	vtgpu_transfer_to_host_2d(sc, 0, 0, sc->vtgpu_fb_info.fb_width,
	    sc->vtgpu_fb_info.fb_height);
	vtgpu_resource_flush(sc, 0, 0, sc->vtgpu_fb_info.fb_width,
	    sc->vtgpu_fb_info.fb_height);
}

static void
vtgpu_fb_bitblt_text(struct vt_device *vd, const struct vt_window *vw,
    const term_rect_t *area)
{
	struct vtgpu_softc *sc;
	struct fb_info *info;
	int x, y, width, height;

	info = vd->vd_softc;
	sc = (struct vtgpu_softc *)info;

	vt_fb_bitblt_text(vd, vw, area);

	x = area->tr_begin.tp_col * vw->vw_font->vf_width + vw->vw_draw_area.tr_begin.tp_col;
	y = area->tr_begin.tp_row * vw->vw_font->vf_height + vw->vw_draw_area.tr_begin.tp_row;
	width = area->tr_end.tp_col * vw->vw_font->vf_width + vw->vw_draw_area.tr_begin.tp_col - x;
	height = area->tr_end.tp_row * vw->vw_font->vf_height + vw->vw_draw_area.tr_begin.tp_row - y;

	vtgpu_transfer_to_host_2d(sc, x, y, width, height);
	vtgpu_resource_flush(sc, x, y, width, height);
}

static void
vtgpu_fb_bitblt_bitmap(struct vt_device *vd, const struct vt_window *vw,
    const uint8_t *pattern, const uint8_t *mask,
    unsigned int width, unsigned int height,
    unsigned int x, unsigned int y, term_color_t fg, term_color_t bg)
{
	struct vtgpu_softc *sc;
	struct fb_info *info;

	info = vd->vd_softc;
	sc = (struct vtgpu_softc *)info;

	vt_fb_bitblt_bitmap(vd, vw, pattern, mask, width, height, x, y, fg, bg);

	vtgpu_transfer_to_host_2d(sc, x, y, width, height);
	vtgpu_resource_flush(sc, x, y, width, height);
}

static int
vtgpu_fb_bitblt_argb(struct vt_device *vd, const struct vt_window *vw,
    const uint8_t *argb,
    unsigned int width, unsigned int height,
    unsigned int x, unsigned int y)
{

	return (EOPNOTSUPP);
}

static void
vtgpu_fb_drawrect(struct vt_device *vd, int x1, int y1, int x2, int y2,
    int fill, term_color_t color)
{
	struct vtgpu_softc *sc;
	struct fb_info *info;
	int width, height;

	info = vd->vd_softc;
	sc = (struct vtgpu_softc *)info;

	vt_fb_drawrect(vd, x1, y1, x2, y2, fill, color);

	width = x2 - x1 + 1;
	height = y2 - y1 + 1;
	vtgpu_transfer_to_host_2d(sc, x1, y1, width, height);
	vtgpu_resource_flush(sc, x1, y1, width, height);
}

static void
vtgpu_fb_setpixel(struct vt_device *vd, int x, int y, term_color_t color)
{
	struct vtgpu_softc *sc;
	struct fb_info *info;

	info = vd->vd_softc;
	sc = (struct vtgpu_softc *)info;

	vt_fb_setpixel(vd, x, y, color);

	vtgpu_transfer_to_host_2d(sc, x, y, 1, 1);
	vtgpu_resource_flush(sc, x, y, 1, 1);
}

static struct virtio_feature_desc vtgpu_feature_desc[] = {
	{ VIRTIO_GPU_F_VIRGL,		"VirGL"		},
	{ VIRTIO_GPU_F_EDID,		"EDID"		},
	{ VIRTIO_GPU_F_RESOURCE_UUID,	"ResUUID"	},
	{ VIRTIO_GPU_F_RESOURCE_BLOB,	"ResBlob"	},
	{ VIRTIO_GPU_F_CONTEXT_INIT,	"ContextInit"	},
	{ 0, NULL }
};

static device_method_t vtgpu_methods[] = {
	/* Device methods. */
	DEVMETHOD(device_probe,		vtgpu_probe),
	DEVMETHOD(device_attach,	vtgpu_attach),
	DEVMETHOD(device_detach,	vtgpu_detach),

	/* VirtIO methods. */
	DEVMETHOD(virtio_config_change,	vtgpu_config_change),

	DEVMETHOD_END
};

static driver_t vtgpu_driver = {
	"vgpu",
	vtgpu_methods,
	sizeof(struct vtgpu_softc)
};

VIRTIO_DRIVER_MODULE(virtio_drm, vtgpu_driver, vtgpu_modevent, NULL);
MODULE_VERSION(virtio_drm, 1);
MODULE_DEPEND(virtio_drm, virtio, 1, 1, 1);
/*
 * No MODULE_DEPEND on drm2 here: in-base FreeBSD drm2 is reached
 * via 'device drm2' in the kernel config (statically linked), not
 * via a separate loadable module.  The kernel must therefore have
 * been built with drm2 — typical for arm64 development kernels
 * that already host rk_drm or similar.  GENERIC does not include
 * drm2; you must add 'device drm2' to a custom KERNCONF.
 */

/*
 * ===========================================================================
 * Phase 3 — DRM-KMS frontend on the in-base drm2 stack.
 * ===========================================================================
 *
 * This is the section that turns virtio_drm from a fbio framebuffer driver
 * into a real DRM-KMS driver: registers /dev/dri/card0, exposes a CRTC,
 * connector, and plane to userland, and lets X11 / Wayland clients drive
 * modesets and submit framebuffers.
 *
 * 3.A (this commit): drm_driver struct + stub callbacks only.  No DRM
 * device is registered yet.  Subsequent commits wire up drm_get_pci_dev or
 * equivalent (3.B), drm_mode_config_init (3.C), CRTC (3.D), connector
 * (3.E), plane + dumb buffers + vt handover (3.F-H).
 * ===========================================================================
 */

/*
 * Phase 3.C/G — drm_mode_config_funcs.
 *
 * fb_create wraps a GEM dumb-buffer handle in a drm_framebuffer.  Once
 * Phase 3.G shipped, this is the path userland's DRM_IOCTL_MODE_ADDFB2
 * takes.  output_poll_changed is invoked by the connector polling
 * machinery on hotplug; with no fbdev emulation it's a no-op.
 */

/* Full vtgpu_gem_bo definition is in the Phase 3.G block below. */
struct vtgpu_gem_bo {
	struct drm_gem_object	gem_obj;
	vm_offset_t		vbase;
	bus_addr_t		pbase;
	size_t			size;
	uint32_t		resource_id;
	bool			host_resource_attached;
};

struct virtio_drm_framebuffer {
	struct drm_framebuffer	base;
	struct vtgpu_gem_bo	*bo;
};

static void
virtio_drm_fb_destroy(struct drm_framebuffer *drm_fb)
{
	struct virtio_drm_framebuffer *fb = (struct virtio_drm_framebuffer *)drm_fb;

	if (fb->bo != NULL)
		drm_gem_object_unreference_unlocked(&fb->bo->gem_obj);
	drm_framebuffer_cleanup(drm_fb);
	free(fb, M_DEVBUF);
}

static int
virtio_drm_fb_create_handle(struct drm_framebuffer *drm_fb,
    struct drm_file *file, unsigned int *handle)
{
	struct virtio_drm_framebuffer *fb = (struct virtio_drm_framebuffer *)drm_fb;
	return (drm_gem_handle_create(file, &fb->bo->gem_obj, handle));
}

static const struct drm_framebuffer_funcs virtio_drm_fb_funcs = {
	.destroy	= virtio_drm_fb_destroy,
	.create_handle	= virtio_drm_fb_create_handle,
};

static int
virtio_drm_fb_create(struct drm_device *ddev, struct drm_file *file,
    struct drm_mode_fb_cmd2 *mode_cmd, struct drm_framebuffer **fb_out)
{
	struct drm_gem_object *gem_obj;
	struct virtio_drm_framebuffer *fb;
	int error;

	gem_obj = drm_gem_object_lookup(ddev, file, mode_cmd->handles[0]);
	if (gem_obj == NULL)
		return (-ENOENT);

	fb = malloc(sizeof(*fb), M_DEVBUF, M_WAITOK | M_ZERO);
	fb->bo = (struct vtgpu_gem_bo *)gem_obj;
	fb->base.pitches[0] = mode_cmd->pitches[0];
	fb->base.offsets[0] = mode_cmd->offsets[0];
	fb->base.width = mode_cmd->width;
	fb->base.height = mode_cmd->height;
	fb->base.depth = 24;
	fb->base.bits_per_pixel = 32;
	error = drm_framebuffer_init(ddev, &fb->base, &virtio_drm_fb_funcs);
	if (error != 0) {
		drm_gem_object_unreference_unlocked(gem_obj);
		free(fb, M_DEVBUF);
		return (error);
	}
	*fb_out = &fb->base;
	return (0);
}

static void
virtio_drm_output_poll_changed(struct drm_device *ddev)
{
	(void)ddev;
}

static const struct drm_mode_config_funcs virtio_drm_mode_config_funcs = {
	.fb_create		= virtio_drm_fb_create,
	.output_poll_changed	= virtio_drm_output_poll_changed,
};

/*
 * Phase 3.D — CRTC.
 *
 * One CRTC per virtio_drm device.  Maps the DRM mode-setting concept
 * onto our existing virtio-gpu commands:
 *
 *   - dpms        : VIRTIO_GPU_CMD_SET_SCANOUT with resource_id=0 to
 *                   blank, or with our framebuffer resource to enable.
 *   - mode_set    : SET_SCANOUT to point the host's scanout at our
 *                   primary fb resource using the new mode dimensions.
 *   - prepare/commit: brackets around mode_set; we have nothing to
 *                     latch atomically so they're no-ops.
 *   - mode_set_base: same as mode_set but only updates origin/fb,
 *                    not timing.
 *
 * Phase 3.E adds the connector that drives mode discovery; until
 * then this CRTC is dormant from userland's perspective.
 */

static void
virtio_drm_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	struct vtgpu_softc *sc;
	uint32_t scanout;

	sc = device_get_softc(crtc->dev->dev);
	scanout = sc->vtgpu_primary_scanout;

	if (mode == DRM_MODE_DPMS_ON) {
		(void)vtgpu_set_scanout(sc, 0, 0,
		    sc->vtgpu_fb_info.fb_width,
		    sc->vtgpu_fb_info.fb_height);
	} else {
		/* Blank: point scanout at resource_id 0 to detach. */
		struct {
			struct virtio_gpu_set_scanout req;
			char pad;
			struct virtio_gpu_ctrl_hdr resp;
		} s = { 0 };
		s.req.hdr.type = htole32(VIRTIO_GPU_CMD_SET_SCANOUT);
		s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
		s.req.hdr.fence_id = htole64(
		    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
		s.req.scanout_id = htole32(scanout);
		s.req.resource_id = htole32(0);
		(void)vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
		    sizeof(s.resp));
	}
}

static bool
virtio_drm_crtc_mode_fixup(struct drm_crtc *crtc,
    const struct drm_display_mode *mode, struct drm_display_mode *adj)
{
	(void)crtc;
	(void)mode;
	(void)adj;
	return (true);
}

static int
virtio_drm_crtc_mode_set(struct drm_crtc *crtc, struct drm_display_mode *mode,
    struct drm_display_mode *adj, int x, int y,
    struct drm_framebuffer *old_fb)
{
	struct vtgpu_softc *sc;
	int error;

	(void)adj;
	(void)x;
	(void)y;
	(void)old_fb;

	sc = device_get_softc(crtc->dev->dev);

	/*
	 * For now we drive only the primary framebuffer resource that
	 * was set up at attach.  Phase 3.G adds the proper plane/fb
	 * binding so userland's drm_framebuffer becomes the scanout
	 * source.  Here we just re-publish the existing fb at whatever
	 * size the mode requests, clipped to what we actually allocated.
	 */
	uint32_t w = mode->hdisplay;
	uint32_t h = mode->vdisplay;
	if (w > sc->vtgpu_fb_info.fb_width)
		w = sc->vtgpu_fb_info.fb_width;
	if (h > sc->vtgpu_fb_info.fb_height)
		h = sc->vtgpu_fb_info.fb_height;

	error = vtgpu_set_scanout(sc, 0, 0, w, h);
	if (error != 0)
		return (error);
	(void)vtgpu_transfer_to_host_2d(sc, 0, 0, w, h);
	(void)vtgpu_resource_flush(sc, 0, 0, w, h);
	return (0);
}

static int
virtio_drm_crtc_mode_set_base(struct drm_crtc *crtc, int x, int y,
    struct drm_framebuffer *old_fb)
{
	/* Same operation as mode_set, minus the timing change. */
	return (virtio_drm_crtc_mode_set(crtc, &crtc->mode, &crtc->mode,
	    x, y, old_fb));
}

static void
virtio_drm_crtc_prepare(struct drm_crtc *crtc)
{
	virtio_drm_crtc_dpms(crtc, DRM_MODE_DPMS_OFF);
}

static void
virtio_drm_crtc_commit(struct drm_crtc *crtc)
{
	virtio_drm_crtc_dpms(crtc, DRM_MODE_DPMS_ON);
}

static const struct drm_crtc_helper_funcs virtio_drm_crtc_helper_funcs = {
	.dpms		= virtio_drm_crtc_dpms,
	.mode_fixup	= virtio_drm_crtc_mode_fixup,
	.mode_set	= virtio_drm_crtc_mode_set,
	.mode_set_base	= virtio_drm_crtc_mode_set_base,
	.prepare	= virtio_drm_crtc_prepare,
	.commit		= virtio_drm_crtc_commit,
};

static void
virtio_drm_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
}

static const struct drm_crtc_funcs virtio_drm_crtc_funcs = {
	.set_config	= drm_crtc_helper_set_config,
	.destroy	= virtio_drm_crtc_destroy,
};

/*
 * Phase 3.E — encoder + connector.
 *
 * VirtIO GPU has no physical encoder, but DRM's modeset model requires
 * one between the connector and the CRTC.  We declare a no-op VIRTUAL
 * encoder whose helper just defers everything to the CRTC.
 *
 * The connector is also VIRTUAL (DRM_MODE_CONNECTOR_VIRTUAL).  Its
 * get_modes() feeds the EDID cached by Phase 1.B into
 * drm_add_edid_modes(), which parses the standard and detailed mode
 * descriptors and adds drm_display_mode entries to the connector's
 * mode list.  mode_valid() clips to mode_config.max_*.  detect()
 * always returns connected — a virtio-gpu scanout doesn't disappear.
 */

/* ----- encoder ----- */
static void
virtio_drm_encoder_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
}
static const struct drm_encoder_funcs virtio_drm_encoder_funcs = {
	.destroy	= virtio_drm_encoder_destroy,
};
static void
virtio_drm_encoder_dpms(struct drm_encoder *e, int mode) { (void)e; (void)mode; }
static bool
virtio_drm_encoder_mode_fixup(struct drm_encoder *e,
    const struct drm_display_mode *m, struct drm_display_mode *a)
{ (void)e; (void)m; (void)a; return (true); }
static void
virtio_drm_encoder_prepare(struct drm_encoder *e) { (void)e; }
static void
virtio_drm_encoder_commit(struct drm_encoder *e) { (void)e; }
static void
virtio_drm_encoder_mode_set(struct drm_encoder *e,
    struct drm_display_mode *m, struct drm_display_mode *a)
{ (void)e; (void)m; (void)a; }
static const struct drm_encoder_helper_funcs virtio_drm_encoder_helper_funcs = {
	.dpms		= virtio_drm_encoder_dpms,
	.mode_fixup	= virtio_drm_encoder_mode_fixup,
	.prepare	= virtio_drm_encoder_prepare,
	.commit		= virtio_drm_encoder_commit,
	.mode_set	= virtio_drm_encoder_mode_set,
};

/* ----- connector ----- */
static int
virtio_drm_connector_get_modes(struct drm_connector *connector)
{
	struct vtgpu_softc *sc;
	struct edid *edid;
	int count = 0;

	sc = device_get_softc(connector->dev->dev);

	if (sc->vtgpu_scanouts[sc->vtgpu_primary_scanout].edid_size > 0) {
		edid = (struct edid *)sc->vtgpu_scanouts[sc->vtgpu_primary_scanout].edid;
		drm_mode_connector_update_edid_property(connector, edid);
		count = drm_add_edid_modes(connector, edid);
	}
	if (count == 0) {
		/*
		 * Host didn't give us a usable EDID — fall back to the
		 * advertised scanout dimensions so userland sees at least
		 * one mode.
		 */
		struct drm_display_mode *mode;
		uint32_t w = sc->vtgpu_scanouts[sc->vtgpu_primary_scanout].width;
		uint32_t h = sc->vtgpu_scanouts[sc->vtgpu_primary_scanout].height;
		if (w == 0 || h == 0) { w = 1024; h = 768; }
		mode = drm_cvt_mode(connector->dev, w, h, 60, false, false, false);
		if (mode != NULL) {
			drm_mode_probed_add(connector, mode);
			count = 1;
		}
	}
	return (count);
}

static int
virtio_drm_connector_mode_valid(struct drm_connector *connector,
    struct drm_display_mode *mode)
{
	if (mode->hdisplay > connector->dev->mode_config.max_width)
		return (MODE_BAD);
	if (mode->vdisplay > connector->dev->mode_config.max_height)
		return (MODE_BAD);
	return (MODE_OK);
}

static struct drm_encoder *
virtio_drm_connector_best_encoder(struct drm_connector *connector)
{
	struct vtgpu_softc *sc = device_get_softc(connector->dev->dev);
	return (&sc->vtgpu_drm_encoder);
}

static const struct drm_connector_helper_funcs
    virtio_drm_connector_helper_funcs = {
	.get_modes	= virtio_drm_connector_get_modes,
	.mode_valid	= virtio_drm_connector_mode_valid,
	.best_encoder	= virtio_drm_connector_best_encoder,
};

static enum drm_connector_status
virtio_drm_connector_detect(struct drm_connector *connector, bool force)
{
	(void)connector;
	(void)force;
	return (connector_status_connected);
}
static void
virtio_drm_connector_destroy(struct drm_connector *connector)
{
	drm_connector_cleanup(connector);
}
static void
virtio_drm_connector_dpms(struct drm_connector *connector, int mode)
{
	(void)connector;
	(void)mode;
}
static const struct drm_connector_funcs virtio_drm_connector_funcs = {
	.dpms		= virtio_drm_connector_dpms,
	.detect		= virtio_drm_connector_detect,
	.fill_modes	= drm_helper_probe_single_connector_modes,
	.destroy	= virtio_drm_connector_destroy,
};

/*
 * Phase 3.G — GEM bo + dumb-buffer alloc + framebuffer wrap.
 *
 * Each userland-requested dumb buffer is backed by:
 *   - a contigmalloc'd guest page range (so we can ATTACH_BACKING
 *     with a single sglist entry — same approach we use for the
 *     attach-time primary fb and the cursor),
 *   - a fresh virtio-gpu resource_id (allocated >= 3),
 *   - a struct drm_gem_object so userland gets a handle and an
 *     mmap path to the contig pages.
 *
 * gem_pager_ops.fault converts an mmap offset into a vm_page_t
 * pointing at the appropriate page of the contig buffer; this
 * lets userland fault-in pages of the buffer on demand.
 */

static int
virtio_drm_bo_create_host_resource(struct vtgpu_softc *sc,
    struct vtgpu_gem_bo *bo, uint32_t width, uint32_t height)
{
	struct {
		struct virtio_gpu_resource_create_2d req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} cs = { 0 };
	struct {
		struct {
			struct virtio_gpu_resource_attach_backing backing;
			struct virtio_gpu_mem_entry mem[1];
		} req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} as = { 0 };
	int error;

	cs.req.hdr.type = htole32(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
	cs.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	cs.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	cs.req.resource_id = htole32(bo->resource_id);
	cs.req.format = htole32(VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
	cs.req.width = htole32(width);
	cs.req.height = htole32(height);
	error = vtgpu_req_resp(sc, &cs.req, sizeof(cs.req), &cs.resp,
	    sizeof(cs.resp));
	if (error != 0 ||
	    cs.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA))
		return (-EIO);

	as.req.backing.hdr.type = htole32(
	    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
	as.req.backing.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	as.req.backing.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	as.req.backing.resource_id = htole32(bo->resource_id);
	as.req.backing.nr_entries = htole32(1);
	as.req.mem[0].addr = htole64(bo->pbase);
	as.req.mem[0].length = htole32(bo->size);
	error = vtgpu_req_resp(sc, &as.req, sizeof(as.req), &as.resp,
	    sizeof(as.resp));
	if (error != 0 ||
	    as.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA))
		return (-EIO);

	bo->host_resource_attached = true;
	return (0);
}

static void
virtio_drm_bo_destroy_host_resource(struct vtgpu_softc *sc,
    struct vtgpu_gem_bo *bo)
{
	struct {
		struct virtio_gpu_resource_detach_backing req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} ds = { 0 };
	struct {
		struct virtio_gpu_resource_unref req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} us = { 0 };

	if (!bo->host_resource_attached)
		return;

	ds.req.hdr.type = htole32(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
	ds.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	ds.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	ds.req.resource_id = htole32(bo->resource_id);
	(void)vtgpu_req_resp(sc, &ds.req, sizeof(ds.req), &ds.resp,
	    sizeof(ds.resp));

	us.req.hdr.type = htole32(VIRTIO_GPU_CMD_RESOURCE_UNREF);
	us.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	us.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	us.req.resource_id = htole32(bo->resource_id);
	(void)vtgpu_req_resp(sc, &us.req, sizeof(us.req), &us.resp,
	    sizeof(us.resp));

	bo->host_resource_attached = false;
}

/* drm_driver.gem_free_object */
static void
virtio_drm_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct vtgpu_gem_bo *bo = (struct vtgpu_gem_bo *)gem_obj;
	struct vtgpu_softc *sc = device_get_softc(gem_obj->dev->dev);

	virtio_drm_bo_destroy_host_resource(sc, bo);
	if (bo->vbase != 0)
		free((void *)bo->vbase, M_DEVBUF);
	drm_gem_object_release(gem_obj);
	free(bo, M_DEVBUF);
}

/* gem_pager_ops: hand back the right page from the contig buffer. */
static int
virtio_drm_gem_pager_fault(vm_object_t vm_obj, vm_ooffset_t offset,
    int prot, vm_page_t *mres)
{
	(void)prot;
	struct drm_gem_object *gem_obj = vm_obj->handle;
	struct vtgpu_gem_bo *bo = (struct vtgpu_gem_bo *)gem_obj;
	vm_page_t page, oldm;
	vm_paddr_t paddr;

	if (offset >= bo->size)
		return (VM_PAGER_FAIL);

	paddr = bo->pbase + offset;
	page = PHYS_TO_VM_PAGE(paddr);
	if (page == NULL)
		return (VM_PAGER_FAIL);
	vm_page_busy_acquire(page, 0);

	oldm = *mres;
	if (oldm != NULL) {
		vm_page_replace(page, vm_obj, oldm->pindex, oldm);
	} else {
		vm_page_insert(page, vm_obj, OFF_TO_IDX(offset));
	}
	page->valid = VM_PAGE_BITS_ALL;
	*mres = page;
	return (VM_PAGER_OK);
}

static int
virtio_drm_gem_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t foff, struct ucred *cred, u_short *color)
{
	(void)handle; (void)size; (void)prot; (void)foff; (void)cred;
	if (color != NULL)
		*color = 0;
	return (0);
}
static void
virtio_drm_gem_pager_dtor(void *handle) { (void)handle; }

static struct cdev_pager_ops virtio_drm_gem_pager_ops = {
	.cdev_pg_fault	= virtio_drm_gem_pager_fault,
	.cdev_pg_ctor	= virtio_drm_gem_pager_ctor,
	.cdev_pg_dtor	= virtio_drm_gem_pager_dtor,
};

/* drm_driver.dumb_create */
static int
virtio_drm_dumb_create(struct drm_file *file_priv, struct drm_device *ddev,
    struct drm_mode_create_dumb *args)
{
	struct vtgpu_softc *sc = device_get_softc(ddev->dev);
	struct vtgpu_gem_bo *bo;
	size_t size;
	int error;

	args->pitch = (args->width * args->bpp + 7) / 8;
	args->pitch = roundup(args->pitch, 64);
	args->size = (uint64_t)args->pitch * args->height;
	size = round_page(args->size);
	if (size == 0)
		return (-EINVAL);

	bo = malloc(sizeof(*bo), M_DEVBUF, M_WAITOK | M_ZERO);
	bo->size = size;
	bo->vbase = (vm_offset_t)contigmalloc(size, M_DEVBUF,
	    M_WAITOK | M_ZERO, 0, ~0, 4, 0);
	if (bo->vbase == 0) {
		free(bo, M_DEVBUF);
		return (-ENOMEM);
	}
	bo->pbase = pmap_kextract(bo->vbase);
	bo->resource_id = atomic_fetchadd_32(&sc->vtgpu_next_resource_id, 1);

	error = drm_gem_object_init(ddev, &bo->gem_obj, size);
	if (error != 0)
		goto err_free_pages;
	error = drm_gem_create_mmap_offset(&bo->gem_obj);
	if (error != 0)
		goto err_gem_release;

	error = virtio_drm_bo_create_host_resource(sc, bo,
	    args->width, args->height);
	if (error != 0)
		goto err_gem_release;

	error = drm_gem_handle_create(file_priv, &bo->gem_obj, &args->handle);
	if (error != 0) {
		virtio_drm_bo_destroy_host_resource(sc, bo);
		goto err_gem_release;
	}
	drm_gem_object_unreference_unlocked(&bo->gem_obj);
	return (0);

err_gem_release:
	drm_gem_object_release(&bo->gem_obj);
err_free_pages:
	free((void *)bo->vbase, M_DEVBUF);
	free(bo, M_DEVBUF);
	return (error);
}

/* drm_driver.dumb_map_offset */
static int
virtio_drm_dumb_map_offset(struct drm_file *file_priv,
    struct drm_device *ddev, uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *gem_obj;
	int error = 0;

	DRM_LOCK(ddev);
	gem_obj = drm_gem_object_lookup(ddev, file_priv, handle);
	if (gem_obj == NULL) {
		DRM_UNLOCK(ddev);
		return (-EINVAL);
	}
	error = drm_gem_create_mmap_offset(gem_obj);
	if (error == 0) {
		*offset = DRM_GEM_MAPPING_OFF(gem_obj->map_list.key) |
		    DRM_GEM_MAPPING_KEY;
	}
	drm_gem_object_unreference(gem_obj);
	DRM_UNLOCK(ddev);
	return (error);
}

/* drm_driver.dumb_destroy */
static int
virtio_drm_dumb_destroy(struct drm_file *file_priv,
    struct drm_device *ddev, uint32_t handle)
{
	return (drm_gem_handle_delete(file_priv, handle));
}

/*
 * Phase 3.H — vt(4) handover.
 *
 * When the first userland process opens /dev/dri/card0, hand the
 * framebuffer to it: tear down vt(4)'s ownership so the console
 * stops drawing characters into our scanout source.  When the last
 * DRM fd closes, restore vt(4) so the console comes back.
 *
 * drm2 fires firstopen() on the very first open and lastclose() once
 * the open-count drops to zero — exactly the brackets we need.
 *
 * Why this matters: without handover, vt(4)'s vd_bitblt_text callback
 * keeps drawing console chars into the same framebuffer pages X (or
 * kmscube) is mapping and writing.  You get garbled output where the
 * X cursor and console cursor fight, and `dmesg` messages from other
 * processes blink over X content.
 */
static int
virtio_drm_drm_firstopen(struct drm_device *ddev)
{
	struct vtgpu_softc *sc = device_get_softc(ddev->dev);

	if (sc->vtgpu_have_fb_info) {
		vt_deallocate(&vtgpu_fb_driver, &sc->vtgpu_fb_info);
		sc->vtgpu_have_fb_info = false;
		device_printf(ddev->dev,
		    "virtio_drm: firstopen — vt(4) detached, DRM owns fb\n");
	}
	return (0);
}

static void
virtio_drm_drm_lastclose(struct drm_device *ddev)
{
	struct vtgpu_softc *sc = device_get_softc(ddev->dev);

	if (!sc->vtgpu_have_fb_info && sc->vtgpu_fb_info.fb_vbase != 0) {
		vt_allocate(&vtgpu_fb_driver, &sc->vtgpu_fb_info);
		sc->vtgpu_have_fb_info = true;
		device_printf(ddev->dev,
		    "virtio_drm: lastclose — vt(4) restored\n");
	}
}

/*
 * Driver-load callback.  drm_get_platform_dev() calls this between
 * drm_get_minor() and drm_mode_group_init_legacy_group(); the latter
 * iterates dev->mode_config.crtc_list, so the mode_config MUST be
 * initialized before load returns or the framework crashes with a
 * NULL deref on a zeroed list head.
 *
 * Phase 3.C: derive sensible bounds from the host-reported scanout
 * topology (cached by Phase 1.A) rather than leaving them at the
 * permissive 16384x16384 we used in 3.B.  Sets up fb_create +
 * output_poll_changed callbacks ready for the CRTC/connector/plane
 * commits that follow.
 */
static int
virtio_drm_drm_load(struct drm_device *ddev, unsigned long flags)
{
	struct vtgpu_softc *sc;
	uint32_t max_w, max_h;
	uint32_t i;

	(void)flags;

	sc = device_get_softc(ddev->dev);

	drm_mode_config_init(ddev);

	/*
	 * Bounds: start at the largest scanout dimension we've seen +
	 * some headroom so dynamic resize via VIRTIO_GPU_EVENT_DISPLAY
	 * within reason doesn't trip max-bounds checks.
	 */
	max_w = 0;
	max_h = 0;
	for (i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
		if (!sc->vtgpu_scanouts[i].enabled)
			continue;
		if (sc->vtgpu_scanouts[i].width > max_w)
			max_w = sc->vtgpu_scanouts[i].width;
		if (sc->vtgpu_scanouts[i].height > max_h)
			max_h = sc->vtgpu_scanouts[i].height;
	}
	if (max_w == 0)
		max_w = 1920;
	if (max_h == 0)
		max_h = 1080;

	ddev->mode_config.min_width = 0;
	ddev->mode_config.min_height = 0;
	ddev->mode_config.max_width = max_w * 4;	/* headroom */
	ddev->mode_config.max_height = max_h * 4;
	ddev->mode_config.funcs = __DECONST(struct drm_mode_config_funcs *,
	    &virtio_drm_mode_config_funcs);

	/*
	 * Phase 3.D: create the single CRTC.  Pass NULL for the primary
	 * plane; Phase 3.F replaces this with a real plane binding.
	 */
	drm_crtc_init(ddev, &sc->vtgpu_drm_crtc, &virtio_drm_crtc_funcs);
	drm_crtc_helper_add(&sc->vtgpu_drm_crtc,
	    &virtio_drm_crtc_helper_funcs);

	/*
	 * Phase 3.E: encoder + connector.  Encoder is VIRTUAL (no real
	 * encoding hardware).  Connector is VIRTUAL and feeds the cached
	 * EDID into the mode list at probe time.
	 */
	sc->vtgpu_drm_encoder.possible_crtcs = 1;	/* CRTC #0 */
	drm_encoder_init(ddev, &sc->vtgpu_drm_encoder,
	    &virtio_drm_encoder_funcs, DRM_MODE_ENCODER_VIRTUAL);
	drm_encoder_helper_add(&sc->vtgpu_drm_encoder,
	    &virtio_drm_encoder_helper_funcs);

	drm_connector_init(ddev, &sc->vtgpu_drm_connector,
	    &virtio_drm_connector_funcs, DRM_MODE_CONNECTOR_VIRTUAL);
	drm_connector_helper_add(&sc->vtgpu_drm_connector,
	    &virtio_drm_connector_helper_funcs);
	drm_mode_connector_attach_encoder(&sc->vtgpu_drm_connector,
	    &sc->vtgpu_drm_encoder);
	sc->vtgpu_drm_connector.encoder = &sc->vtgpu_drm_encoder;

	device_printf(ddev->dev,
	    "virtio_drm: drm_load mode_config bounds %ux%u..%ux%u, "
	    "1 CRTC + 1 connector ready\n",
	    ddev->mode_config.min_width, ddev->mode_config.min_height,
	    ddev->mode_config.max_width, ddev->mode_config.max_height);
	return (0);
}

/* Driver-unload callback — symmetric to load. */
static int
virtio_drm_drm_unload(struct drm_device *ddev)
{
	drm_mode_config_cleanup(ddev);
	device_printf(ddev->dev, "virtio_drm: drm_unload\n");
	return (0);
}

/*
 * VBLANK callbacks.  Virtio-gpu has no real vblank — the host renders the
 * framebuffer when we issue RESOURCE_FLUSH.  Return synthetic-counter
 * values so userland sees consistent monotonic counts.
 */
static u32
virtio_drm_get_vblank_counter(struct drm_device *ddev, int crtc)
{
	(void)ddev;
	(void)crtc;
	return (0);
}

static int
virtio_drm_enable_vblank(struct drm_device *ddev, int crtc)
{
	(void)ddev;
	(void)crtc;
	return (0);
}

static void
virtio_drm_disable_vblank(struct drm_device *ddev, int crtc)
{
	(void)ddev;
	(void)crtc;
}

/* Empty ioctl table — DRM core ioctls handle the modesetting API. */
static const struct drm_ioctl_desc virtio_drm_ioctls[] = {
	/* per-driver ioctls land here in later phases */
};

static struct drm_driver virtio_drm_drm_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_GEM,
	.load			= virtio_drm_drm_load,
	.unload			= virtio_drm_drm_unload,
	.firstopen		= virtio_drm_drm_firstopen,
	.lastclose		= virtio_drm_drm_lastclose,
	.get_vblank_counter	= virtio_drm_get_vblank_counter,
	.enable_vblank		= virtio_drm_enable_vblank,
	.disable_vblank		= virtio_drm_disable_vblank,
	.gem_free_object	= virtio_drm_gem_free_object,
	.gem_pager_ops		= &virtio_drm_gem_pager_ops,
	.dumb_create		= virtio_drm_dumb_create,
	.dumb_map_offset	= virtio_drm_dumb_map_offset,
	.dumb_destroy		= virtio_drm_dumb_destroy,
	.ioctls			= __DECONST(struct drm_ioctl_desc *,
				    virtio_drm_ioctls),
	.num_ioctls		= nitems(virtio_drm_ioctls),
	.name			= VIRTIO_DRM_DRIVER_NAME,
	.desc			= VIRTIO_DRM_DRIVER_DESC,
	.date			= VIRTIO_DRM_DRIVER_DATE,
	.major			= VIRTIO_DRM_DRIVER_MAJOR,
	.minor			= VIRTIO_DRM_DRIVER_MINOR,
};

VIRTIO_SIMPLE_PNPINFO(virtio_drm, VIRTIO_ID_GPU,
    "VirtIO GPU (virtio_drm)");

static int
vtgpu_modevent(module_t mod, int type, void *unused)
{
	int error;

	switch (type) {
	case MOD_LOAD:
	case MOD_QUIESCE:
	case MOD_UNLOAD:
	case MOD_SHUTDOWN:
		error = 0;
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

/* Match the VirtIO GPU device-type ID and outrank the in-base driver. */
static int
vtgpu_probe(device_t dev)
{
	int rv;

	rv = VIRTIO_SIMPLE_PROBE(dev, virtio_drm);
	if (rv != BUS_PROBE_DEFAULT)
		return (rv);

	/*
	 * Win the probe race against the in-base virtio_gpu driver
	 * when both are present.  Both register on the virtio_pci bus
	 * with BUS_PROBE_DEFAULT (-20); BUS_PROBE_VENDOR (-10) ranks
	 * higher and lets the loadable module take ownership without
	 * needing the user to do a runtime detach/rescan dance.
	 */
	return (BUS_PROBE_VENDOR);
}

/* Bring the device up: negotiate features, fetch displays + EDID, set scanout 0. */
static int
vtgpu_attach(device_t dev)
{
	struct vtgpu_softc *sc;
	int error;

	sc = device_get_softc(dev);
	sc->vtgpu_have_fb_info = false;
	sc->vtgpu_dev = dev;
	sc->vtgpu_next_fence = 1;
	sc->vtgpu_next_resource_id = 3;	/* 1=primary fb, 2=cursor */
	virtio_set_feature_desc(dev, vtgpu_feature_desc);

	error = vtgpu_setup_features(sc);
	if (error != 0) {
		device_printf(dev, "cannot setup features\n");
		goto fail;
	}

	vtgpu_read_config(sc, &sc->vtgpu_gpucfg);

	error = vtgpu_alloc_virtqueue(sc);
	if (error != 0) {
		device_printf(dev, "cannot allocate virtqueue\n");
		goto fail;
	}

	virtio_setup_intr(dev, INTR_TYPE_TTY);

	/* Read the device info to get the display size */
	error = vtgpu_get_display_info(sc);
	if (error != 0) {
		goto fail;
	}

	/*
	 * Best-effort EDID fetch for every enabled scanout.  Errors
	 * here are non-fatal: hosts that don't have an EDID for a
	 * given scanout reply INVALID_PARAMETER, and the scanout
	 * still works without one.
	 */
	if (sc->vtgpu_features & (1ULL << VIRTIO_GPU_F_EDID)) {
		for (uint32_t i = 0; i < sc->vtgpu_gpucfg.num_scanouts &&
		    i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
			if (!sc->vtgpu_scanouts[i].enabled)
				continue;
			if (vtgpu_get_edid(sc, i) == 0) {
				device_printf(dev,
				    "scanout %u: EDID %u bytes\n",
				    i, sc->vtgpu_scanouts[i].edid_size);
			}
		}
	}

	/*
	 * TODO: This doesn't need to be contigmalloc as we
	 * can use scatter-gather lists.
	 */
	sc->vtgpu_fb_info.fb_vbase = (vm_offset_t)contigmalloc(
	    sc->vtgpu_fb_info.fb_size, M_DEVBUF, M_WAITOK|M_ZERO, 0, ~0, 4, 0);
	sc->vtgpu_fb_info.fb_pbase = pmap_kextract(sc->vtgpu_fb_info.fb_vbase);

	/* Create the 2d resource */
	error = vtgpu_create_2d(sc);
	if (error != 0) {
		goto fail;
	}
	sc->vtgpu_have_resource = true;

	/* Attach the backing memory */
	error = vtgpu_attach_backing(sc);
	if (error != 0) {
		goto fail;
	}
	sc->vtgpu_have_backing = true;

	/* Set the scanout to link the framebuffer to the display scanout */
	error = vtgpu_set_scanout(sc, 0, 0, sc->vtgpu_fb_info.fb_width,
	    sc->vtgpu_fb_info.fb_height);
	if (error != 0) {
		goto fail;
	}

	vt_allocate(&vtgpu_fb_driver, &sc->vtgpu_fb_info);
	sc->vtgpu_have_fb_info = true;

	error = vtgpu_transfer_to_host_2d(sc, 0, 0, sc->vtgpu_fb_info.fb_width,
	    sc->vtgpu_fb_info.fb_height);
	if (error != 0)
		goto fail;
	error = vtgpu_resource_flush(sc, 0, 0, sc->vtgpu_fb_info.fb_width,
	    sc->vtgpu_fb_info.fb_height);
	if (error != 0)
		goto fail;

	/*
	 * Phase 2: set up the hardware cursor resource.  Failures here
	 * are non-fatal — the primary framebuffer path is already up,
	 * and a missing cursor just means UPDATE_CURSOR/MOVE_CURSOR
	 * calls will fail gracefully later.
	 */
	sc->vtgpu_cursor_vbase = (vm_offset_t)contigmalloc(VTGPU_CURSOR_SIZE,
	    M_DEVBUF, M_WAITOK | M_ZERO, 0, ~0, 4, 0);
	if (sc->vtgpu_cursor_vbase != 0) {
		sc->vtgpu_cursor_pbase =
		    pmap_kextract(sc->vtgpu_cursor_vbase);
		if (vtgpu_cursor_create_resource(sc) == 0) {
			sc->vtgpu_have_cursor_resource = true;
			if (vtgpu_cursor_attach_backing(sc) == 0)
				sc->vtgpu_have_cursor_backing = true;
		}
	}
	if (sc->vtgpu_have_cursor_backing)
		device_printf(dev, "cursor: 64x64 BGRA resource ready\n");

	/* Phase 2.D test surface — dev.vgpu.<unit>.cursor_xy / _test_pattern. */
	vtgpu_sysctl_setup(sc);

	/*
	 * Phase 3.B: register a drm_device so /dev/dri/card0 appears.
	 * Failure is non-fatal — the fbio framebuffer + cursor paths
	 * are already up and useful without DRM-KMS, and a missing
	 * /dev/dri/card0 is recoverable (rebuild with 'device drm2',
	 * reload).  Subsequent phases (3.C-H) wire CRTC/connector/
	 * plane/dumb buffers onto this drm_device.
	 */
	{
		struct drm_device *ddev;
		int derr;

		ddev = malloc(sizeof(*ddev), M_DEVBUF, M_WAITOK | M_ZERO);
		derr = drm_get_platform_dev(dev, ddev,
		    &virtio_drm_drm_driver);
		if (derr == 0) {
			sc->vtgpu_drm_dev = ddev;
			device_printf(dev,
			    "drm: registered /dev/dri/card%d\n",
			    ddev->primary ? ddev->primary->index : 0);
		} else {
			free(ddev, M_DEVBUF);
			device_printf(dev,
			    "drm: drm_get_platform_dev failed: %d "
			    "(is 'device drm2' in your KERNCONF?)\n",
			    derr);
		}
	}

	error = 0;

fail:
	if (error != 0)
		vtgpu_detach(dev);

	return (error);
}

/* Release host-side resource (DETACH_BACKING + UNREF) before freeing pages. */
static int
vtgpu_detach(device_t dev)
{
	struct vtgpu_softc *sc;

	sc = device_get_softc(dev);

	/*
	 * Phase 3.B: unregister DRM device first so userland clients
	 * see EBADF on /dev/dri/card0 before the underlying hardware
	 * state is torn down.
	 */
	if (sc->vtgpu_drm_dev != NULL) {
		drm_put_dev(sc->vtgpu_drm_dev);
		/* drm_put_dev frees the drm_device. */
		sc->vtgpu_drm_dev = NULL;
	}

	if (sc->vtgpu_have_fb_info)
		vt_deallocate(&vtgpu_fb_driver, &sc->vtgpu_fb_info);

	/*
	 * Tear down the host-side resource state before freeing the
	 * guest memory that backs it.  Order matters:
	 *   1. DETACH_BACKING: host stops referencing our pages.
	 *   2. RESOURCE_UNREF: host drops the resource record.
	 *   3. Free the backing pages.
	 * Doing it in the other order leaves the host with dangling
	 * sglist entries pointing at memory the guest has already
	 * returned to the allocator.
	 */
	if (sc->vtgpu_have_backing) {
		(void)vtgpu_detach_backing(sc);
		sc->vtgpu_have_backing = false;
	}
	if (sc->vtgpu_have_resource) {
		(void)vtgpu_resource_unref(sc);
		sc->vtgpu_have_resource = false;
	}

	/* Same teardown order for the cursor resource. */
	if (sc->vtgpu_have_cursor_backing) {
		(void)vtgpu_cursor_detach_backing(sc);
		sc->vtgpu_have_cursor_backing = false;
	}
	if (sc->vtgpu_have_cursor_resource) {
		(void)vtgpu_cursor_resource_unref(sc);
		sc->vtgpu_have_cursor_resource = false;
	}
	if (sc->vtgpu_cursor_vbase != 0) {
		free((void *)sc->vtgpu_cursor_vbase, M_DEVBUF);
		sc->vtgpu_cursor_vbase = 0;
	}

	if (sc->vtgpu_fb_info.fb_vbase != 0) {
		MPASS(sc->vtgpu_fb_info.fb_size != 0);
		free((void *)sc->vtgpu_fb_info.fb_vbase,
		    M_DEVBUF);
	}

	return (0);
}

/*
 * Called by the virtio framework when the device asserts a
 * config-change interrupt.  For virtio-gpu the only event
 * defined today is VIRTIO_GPU_EVENT_DISPLAY (scanout topology
 * changed): the spec wants us to re-fetch display_info, then
 * write the bits we observed back into events_clear so the host
 * knows we noticed.
 *
 * Dynamic fb resize is intentionally NOT done here — vt(4) is
 * a fixed-size console framebuffer, and resizing it under the
 * vt locking model is fragile.  EDID-driven mode changes for
 * userland (X11 / DRM clients) land in Phase 3 along with the
 * DRM-KMS frontend.  For now we just keep the cached
 * display_info + EDID up to date and log the change, which is
 * already useful for diagnosing hotplug.
 */
static int
vtgpu_config_change(device_t dev)
{
	struct vtgpu_softc *sc;
	uint32_t events;
	uint32_t cleared;

	sc = device_get_softc(dev);

	vtgpu_read_config(sc, &sc->vtgpu_gpucfg);
	events = sc->vtgpu_gpucfg.events_read;
	if (events == 0)
		return (0);

	device_printf(dev, "config change: events_read=0x%08x\n", events);

	cleared = 0;
	if (events & VIRTIO_GPU_EVENT_DISPLAY) {
		if (vtgpu_get_display_info(sc) == 0) {
			if (sc->vtgpu_features &
			    (1ULL << VIRTIO_GPU_F_EDID)) {
				for (uint32_t i = 0;
				    i < sc->vtgpu_gpucfg.num_scanouts &&
				    i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
					sc->vtgpu_scanouts[i].edid_size = 0;
					if (sc->vtgpu_scanouts[i].enabled)
						(void)vtgpu_get_edid(sc, i);
				}
			}
		}
		cleared |= VIRTIO_GPU_EVENT_DISPLAY;
	}

	if (cleared != 0) {
		virtio_write_device_config(dev,
		    offsetof(struct virtio_gpu_config, events_clear),
		    &cleared, sizeof(cleared));
	}
	return (0);
}

static int
vtgpu_negotiate_features(struct vtgpu_softc *sc)
{
	device_t dev;
	uint64_t features;

	dev = sc->vtgpu_dev;
	features = VTGPU_FEATURES;

	sc->vtgpu_features = virtio_negotiate_features(dev, features);
	return (virtio_finalize_features(dev));
}

static int
vtgpu_setup_features(struct vtgpu_softc *sc)
{
	int error;

	error = vtgpu_negotiate_features(sc);
	if (error != 0)
		return (error);

	return (0);
}

static void
vtgpu_read_config(struct vtgpu_softc *sc,
    struct virtio_gpu_config *gpucfg)
{
	device_t dev;

	dev = sc->vtgpu_dev;

	bzero(gpucfg, sizeof(struct virtio_gpu_config));

#define VTGPU_GET_CONFIG(_dev, _field, _cfg)			\
	virtio_read_device_config(_dev,				\
	    offsetof(struct virtio_gpu_config, _field),	\
	    &(_cfg)->_field, sizeof((_cfg)->_field))		\

	VTGPU_GET_CONFIG(dev, events_read, gpucfg);
	VTGPU_GET_CONFIG(dev, events_clear, gpucfg);
	VTGPU_GET_CONFIG(dev, num_scanouts, gpucfg);
	VTGPU_GET_CONFIG(dev, num_capsets, gpucfg);

#undef VTGPU_GET_CONFIG
}

/* Allocate the control vq (#0) and the cursor vq (#1). */
static int
vtgpu_alloc_virtqueue(struct vtgpu_softc *sc)
{
	device_t dev;
	struct vq_alloc_info vq_info[2];
	int nvqs;

	dev = sc->vtgpu_dev;
	nvqs = 2;

	VQ_ALLOC_INFO_INIT(&vq_info[0], 0, NULL, sc, &sc->vtgpu_ctrl_vq,
	    "%s control", device_get_nameunit(dev));
	VQ_ALLOC_INFO_INIT(&vq_info[1], 0, NULL, sc, &sc->vtgpu_cursor_vq,
	    "%s cursor", device_get_nameunit(dev));

	return (virtio_alloc_virtqueues(dev, nvqs, vq_info));
}

static int
vtgpu_req_resp(struct vtgpu_softc *sc, void *req, size_t reqlen,
    void *resp, size_t resplen)
{
	struct sglist sg;
	struct sglist_seg segs[2];
	int error;

	sglist_init(&sg, 2, segs);

	error = sglist_append(&sg, req, reqlen);
	if (error != 0) {
		device_printf(sc->vtgpu_dev,
		    "Unable to append the request to the sglist: %d\n", error);
		return (error);
	}
	error = sglist_append(&sg, resp, resplen);
	if (error != 0) {
		device_printf(sc->vtgpu_dev,
		    "Unable to append the response buffer to the sglist: %d\n",
		    error);
		return (error);
	}
	error = virtqueue_enqueue(sc->vtgpu_ctrl_vq, resp, &sg, 1, 1);
	if (error != 0) {
		device_printf(sc->vtgpu_dev, "Enqueue failed: %d\n", error);
		return (error);
	}

	virtqueue_notify(sc->vtgpu_ctrl_vq);
	virtqueue_poll(sc->vtgpu_ctrl_vq, NULL);

	return (0);
}

/*
 * Send an UPDATE_CURSOR / MOVE_CURSOR over the cursor vq.  The spec
 * says the cursor vq is *write-only* — the host does not produce a
 * response — so we don't sglist-append a response buffer.  We still
 * poll for completion to keep the queue from accumulating descriptors,
 * but we expect zero return data.
 */
static int
vtgpu_cursor_post(struct vtgpu_softc *sc, struct virtio_gpu_update_cursor *cmd)
{
	struct sglist sg;
	struct sglist_seg segs[1];
	int error;

	sglist_init(&sg, 1, segs);
	error = sglist_append(&sg, cmd, sizeof(*cmd));
	if (error != 0) {
		device_printf(sc->vtgpu_dev,
		    "cursor: unable to append req to sglist: %d\n", error);
		return (error);
	}

	error = virtqueue_enqueue(sc->vtgpu_cursor_vq, cmd, &sg, 1, 0);
	if (error != 0) {
		device_printf(sc->vtgpu_dev, "cursor: enqueue failed: %d\n",
		    error);
		return (error);
	}
	virtqueue_notify(sc->vtgpu_cursor_vq);
	virtqueue_poll(sc->vtgpu_cursor_vq, NULL);
	return (0);
}

/* VIRTIO_GPU_CMD_GET_DISPLAY_INFO — enumerate all enabled scanouts. */
static int
vtgpu_get_display_info(struct vtgpu_softc *sc)
{
	struct {
		struct virtio_gpu_ctrl_hdr req;
		char pad;
		struct virtio_gpu_resp_display_info resp;
	} s = { 0 };
	int error;

	s.req.type = htole32(VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
	s.req.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.fence_id = htole64(atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);

	uint32_t nscanouts = sc->vtgpu_gpucfg.num_scanouts;
	if (nscanouts > VIRTIO_GPU_MAX_SCANOUTS)
		nscanouts = VIRTIO_GPU_MAX_SCANOUTS;

	int primary = -1;
	for (uint32_t i = 0; i < nscanouts; i++) {
		struct vtgpu_scanout *so = &sc->vtgpu_scanouts[i];
		so->enabled = (s.resp.pmodes[i].enabled != 0);
		so->x = le32toh(s.resp.pmodes[i].r.x);
		so->y = le32toh(s.resp.pmodes[i].r.y);
		so->width = le32toh(s.resp.pmodes[i].r.width);
		so->height = le32toh(s.resp.pmodes[i].r.height);
		so->flags = le32toh(s.resp.pmodes[i].flags);
		if (so->enabled) {
			device_printf(sc->vtgpu_dev,
			    "scanout %u: %ux%u+%u+%u flags=0x%x\n",
			    i, so->width, so->height, so->x, so->y,
			    so->flags);
			if (primary < 0)
				primary = (int)i;
		}
	}

	if (primary < 0) {
		device_printf(sc->vtgpu_dev,
		    "no enabled scanout reported by host\n");
		return (ENXIO);
	}

	sc->vtgpu_primary_scanout = (uint32_t)primary;

	struct vtgpu_scanout *so = &sc->vtgpu_scanouts[primary];
	sc->vtgpu_fb_info.fb_name = device_get_nameunit(sc->vtgpu_dev);
	sc->vtgpu_fb_info.fb_width = so->width;
	sc->vtgpu_fb_info.fb_height = so->height;
	/* 32 bits per pixel */
	sc->vtgpu_fb_info.fb_bpp = 32;
	sc->vtgpu_fb_info.fb_depth = 32;
	sc->vtgpu_fb_info.fb_size = so->width * so->height * 4;
	sc->vtgpu_fb_info.fb_stride = so->width * 4;
	return (0);
}

/* VIRTIO_GPU_CMD_GET_EDID — fetch EDID for one scanout into the softc cache. */
static int
vtgpu_get_edid(struct vtgpu_softc *sc, uint32_t scanout)
{
	struct {
		struct virtio_gpu_cmd_get_edid req;
		char pad;
		struct virtio_gpu_resp_edid resp;
	} s = { 0 };
	int error;
	uint32_t size;
	struct vtgpu_scanout *so;

	if (scanout >= VIRTIO_GPU_MAX_SCANOUTS)
		return (EINVAL);
	if ((sc->vtgpu_features & (1ULL << VIRTIO_GPU_F_EDID)) == 0)
		return (ENOTSUP);

	s.req.hdr.type = htole32(VIRTIO_GPU_CMD_GET_EDID);
	s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	s.req.scanout = htole32(scanout);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);

	if (s.resp.hdr.type != htole32(VIRTIO_GPU_RESP_OK_EDID)) {
		/*
		 * Hosts that don't have an EDID for the scanout reply
		 * VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER; that is not a
		 * hard error — the scanout still works, it just has
		 * no EDID to report.
		 */
		return (ENOENT);
	}

	size = le32toh(s.resp.size);
	if (size > sizeof(s.resp.edid))
		size = sizeof(s.resp.edid);

	so = &sc->vtgpu_scanouts[scanout];
	so->edid_size = size;
	memcpy(so->edid, s.resp.edid, size);

	return (0);
}

static int
vtgpu_create_2d(struct vtgpu_softc *sc)
{
	struct {
		struct virtio_gpu_resource_create_2d req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.hdr.type = htole32(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
	s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));

	s.req.resource_id = htole32(VTGPU_RESOURCE_ID);
	s.req.format = htole32(VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
	s.req.width = htole32(sc->vtgpu_fb_info.fb_width);
	s.req.height = htole32(sc->vtgpu_fb_info.fb_height);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);

	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA)) {
		device_printf(sc->vtgpu_dev, "Invalid reponse type %x\n",
		    le32toh(s.resp.type));
		return (EINVAL);
	}

	return (0);
}

static int
vtgpu_attach_backing(struct vtgpu_softc *sc)
{
	struct {
		struct {
			struct virtio_gpu_resource_attach_backing backing;
			struct virtio_gpu_mem_entry mem[1];
		} req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.backing.hdr.type =
	    htole32(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
	s.req.backing.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.backing.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));

	s.req.backing.resource_id = htole32(VTGPU_RESOURCE_ID);
	s.req.backing.nr_entries = htole32(1);

	s.req.mem[0].addr = htole64(sc->vtgpu_fb_info.fb_pbase);
	s.req.mem[0].length = htole32(sc->vtgpu_fb_info.fb_size);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);

	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA)) {
		device_printf(sc->vtgpu_dev, "Invalid reponse type %x\n",
		    le32toh(s.resp.type));
		return (EINVAL);
	}

	return (0);
}

/* VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING — host drops sglist on our pages. */
static int
vtgpu_detach_backing(struct vtgpu_softc *sc)
{
	struct {
		struct virtio_gpu_resource_detach_backing req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.hdr.type = htole32(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
	s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	s.req.resource_id = htole32(VTGPU_RESOURCE_ID);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);

	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA)) {
		device_printf(sc->vtgpu_dev,
		    "RESOURCE_DETACH_BACKING bad response 0x%x\n",
		    le32toh(s.resp.type));
		return (EINVAL);
	}
	return (0);
}

/* VIRTIO_GPU_CMD_RESOURCE_UNREF — host drops the 2D resource record. */
static int
vtgpu_resource_unref(struct vtgpu_softc *sc)
{
	struct {
		struct virtio_gpu_resource_unref req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.hdr.type = htole32(VIRTIO_GPU_CMD_RESOURCE_UNREF);
	s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	s.req.resource_id = htole32(VTGPU_RESOURCE_ID);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);

	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA)) {
		device_printf(sc->vtgpu_dev,
		    "RESOURCE_UNREF bad response 0x%x\n",
		    le32toh(s.resp.type));
		return (EINVAL);
	}
	return (0);
}

/* RESOURCE_CREATE_2D for the 64x64 BGRA cursor resource. */
static int
vtgpu_cursor_create_resource(struct vtgpu_softc *sc)
{
	struct {
		struct virtio_gpu_resource_create_2d req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.hdr.type = htole32(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
	s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	s.req.resource_id = htole32(VTGPU_CURSOR_RESOURCE_ID);
	s.req.format = htole32(VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM);
	s.req.width = htole32(VTGPU_CURSOR_WIDTH);
	s.req.height = htole32(VTGPU_CURSOR_HEIGHT);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);
	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA)) {
		device_printf(sc->vtgpu_dev,
		    "cursor CREATE_2D bad response 0x%x\n",
		    le32toh(s.resp.type));
		return (EINVAL);
	}
	return (0);
}

/* RESOURCE_ATTACH_BACKING for the cursor resource. */
static int
vtgpu_cursor_attach_backing(struct vtgpu_softc *sc)
{
	struct {
		struct {
			struct virtio_gpu_resource_attach_backing backing;
			struct virtio_gpu_mem_entry mem[1];
		} req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.backing.hdr.type =
	    htole32(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
	s.req.backing.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.backing.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	s.req.backing.resource_id = htole32(VTGPU_CURSOR_RESOURCE_ID);
	s.req.backing.nr_entries = htole32(1);
	s.req.mem[0].addr = htole64(sc->vtgpu_cursor_pbase);
	s.req.mem[0].length = htole32(VTGPU_CURSOR_SIZE);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);
	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA)) {
		device_printf(sc->vtgpu_dev,
		    "cursor ATTACH_BACKING bad response 0x%x\n",
		    le32toh(s.resp.type));
		return (EINVAL);
	}
	return (0);
}

/* RESOURCE_DETACH_BACKING for the cursor resource (teardown). */
static int
vtgpu_cursor_detach_backing(struct vtgpu_softc *sc)
{
	struct {
		struct virtio_gpu_resource_detach_backing req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.hdr.type = htole32(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
	s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	s.req.resource_id = htole32(VTGPU_CURSOR_RESOURCE_ID);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);
	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA))
		return (EINVAL);
	return (0);
}

/* RESOURCE_UNREF for the cursor resource (teardown). */
static int
vtgpu_cursor_resource_unref(struct vtgpu_softc *sc)
{
	struct {
		struct virtio_gpu_resource_unref req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.hdr.type = htole32(VIRTIO_GPU_CMD_RESOURCE_UNREF);
	s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	s.req.resource_id = htole32(VTGPU_CURSOR_RESOURCE_ID);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);
	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA))
		return (EINVAL);
	return (0);
}

/* TRANSFER_TO_HOST_2D for the cursor — push our bitmap to the host. */
static int
vtgpu_cursor_transfer(struct vtgpu_softc *sc)
{
	struct {
		struct virtio_gpu_transfer_to_host_2d req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.hdr.type = htole32(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
	s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	s.req.r.x = htole32(0);
	s.req.r.y = htole32(0);
	s.req.r.width = htole32(VTGPU_CURSOR_WIDTH);
	s.req.r.height = htole32(VTGPU_CURSOR_HEIGHT);
	s.req.offset = htole64(0);
	s.req.resource_id = htole32(VTGPU_CURSOR_RESOURCE_ID);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);
	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA))
		return (EINVAL);
	return (0);
}

/*
 * VIRTIO_GPU_CMD_UPDATE_CURSOR — refresh the cursor bitmap *and*
 * its position.  Pushes our 64x64 BGRA buffer to the host via
 * TRANSFER_TO_HOST_2D first (so the host has the current bits),
 * then posts the UPDATE_CURSOR on the cursor vq.  Callers that
 * only want to move the cursor should use vtgpu_move_cursor()
 * instead — it skips the transfer and uses the fast-path command.
 */
static int
vtgpu_update_cursor(struct vtgpu_softc *sc, uint32_t scanout,
    uint32_t x, uint32_t y, uint32_t hot_x, uint32_t hot_y)
{
	struct virtio_gpu_update_cursor cmd = { 0 };
	int error;

	if (!sc->vtgpu_have_cursor_backing)
		return (ENXIO);

	error = vtgpu_cursor_transfer(sc);
	if (error != 0)
		return (error);

	cmd.hdr.type = htole32(VIRTIO_GPU_CMD_UPDATE_CURSOR);
	cmd.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	cmd.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	cmd.pos.scanout_id = htole32(scanout);
	cmd.pos.x = htole32(x);
	cmd.pos.y = htole32(y);
	cmd.resource_id = htole32(VTGPU_CURSOR_RESOURCE_ID);
	cmd.hot_x = htole32(hot_x);
	cmd.hot_y = htole32(hot_y);

	sc->vtgpu_cursor_x = x;
	sc->vtgpu_cursor_y = y;
	return (vtgpu_cursor_post(sc, &cmd));
}

/*
 * VIRTIO_GPU_CMD_MOVE_CURSOR — fast-path position update.
 * Same struct, but resource_id is unused and no bitmap transfer
 * is performed.  This is what fires at mouse-move frequency.
 */
static int
vtgpu_move_cursor(struct vtgpu_softc *sc, uint32_t scanout,
    uint32_t x, uint32_t y)
{
	struct virtio_gpu_update_cursor cmd = { 0 };

	if (!sc->vtgpu_have_cursor_backing)
		return (ENXIO);

	cmd.hdr.type = htole32(VIRTIO_GPU_CMD_MOVE_CURSOR);
	cmd.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	cmd.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));
	cmd.pos.scanout_id = htole32(scanout);
	cmd.pos.x = htole32(x);
	cmd.pos.y = htole32(y);
	/* resource_id, hot_x, hot_y intentionally zero. */

	sc->vtgpu_cursor_x = x;
	sc->vtgpu_cursor_y = y;
	return (vtgpu_cursor_post(sc, &cmd));
}

/* `dev.vgpu.0.cursor_xy="X Y"` → MOVE_CURSOR. */
static int
vtgpu_sysctl_cursor_xy(SYSCTL_HANDLER_ARGS)
{
	struct vtgpu_softc *sc = arg1;
	char buf[32];
	uint32_t x, y;
	int error;

	snprintf(buf, sizeof(buf), "%u %u", sc->vtgpu_cursor_x,
	    sc->vtgpu_cursor_y);
	error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (sscanf(buf, "%u %u", &x, &y) != 2 &&
	    sscanf(buf, "%u,%u", &x, &y) != 2)
		return (EINVAL);
	return (vtgpu_move_cursor(sc, sc->vtgpu_primary_scanout, x, y));
}

/*
 * `dev.vgpu.0.cursor_test_pattern=1` → paint an opaque-square
 * test bitmap into the cursor buffer and send UPDATE_CURSOR.
 * Useful to confirm end-to-end cursor visibility without DRM.
 */
static int
vtgpu_sysctl_cursor_test(SYSCTL_HANDLER_ARGS)
{
	struct vtgpu_softc *sc = arg1;
	int val = 0;
	int error;
	uint32_t *px;
	uint32_t i;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val == 0 || !sc->vtgpu_have_cursor_backing)
		return (0);

	/* Solid magenta square with a transparent 1-pixel border. */
	px = (uint32_t *)sc->vtgpu_cursor_vbase;
	for (i = 0; i < VTGPU_CURSOR_WIDTH * VTGPU_CURSOR_HEIGHT; i++) {
		uint32_t r = i / VTGPU_CURSOR_WIDTH;
		uint32_t c = i % VTGPU_CURSOR_WIDTH;
		if (r == 0 || c == 0 || r == VTGPU_CURSOR_HEIGHT - 1 ||
		    c == VTGPU_CURSOR_WIDTH - 1)
			px[i] = 0x00000000;	/* transparent border */
		else
			px[i] = 0xffff00ff;	/* BGRA: opaque magenta */
	}
	return (vtgpu_update_cursor(sc, sc->vtgpu_primary_scanout,
	    sc->vtgpu_cursor_x, sc->vtgpu_cursor_y, 0, 0));
}

/* Register the cursor sysctls under the device's auto-created tree. */
static void
vtgpu_sysctl_setup(struct vtgpu_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *oid;

	ctx = device_get_sysctl_ctx(sc->vtgpu_dev);
	oid = device_get_sysctl_tree(sc->vtgpu_dev);
	SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(oid), OID_AUTO, "cursor_xy",
	    CTLTYPE_STRING | CTLFLAG_RW, sc, 0, vtgpu_sysctl_cursor_xy, "A",
	    "Cursor X Y position; write to issue MOVE_CURSOR");
	SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(oid), OID_AUTO,
	    "cursor_test_pattern", CTLTYPE_INT | CTLFLAG_WR, sc, 0,
	    vtgpu_sysctl_cursor_test, "I",
	    "Write 1 to install a magenta test bitmap + UPDATE_CURSOR");
}

static int
vtgpu_set_scanout(struct vtgpu_softc *sc, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
	struct {
		struct virtio_gpu_set_scanout req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.hdr.type = htole32(VIRTIO_GPU_CMD_SET_SCANOUT);
	s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));

	s.req.r.x = htole32(x);
	s.req.r.y = htole32(y);
	s.req.r.width = htole32(width);
	s.req.r.height = htole32(height);

	s.req.scanout_id = 0;
	s.req.resource_id = htole32(VTGPU_RESOURCE_ID);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);

	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA)) {
		device_printf(sc->vtgpu_dev, "Invalid reponse type %x\n",
		    le32toh(s.resp.type));
		return (EINVAL);
	}

	return (0);
}

static int
vtgpu_transfer_to_host_2d(struct vtgpu_softc *sc, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
	struct {
		struct virtio_gpu_transfer_to_host_2d req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.hdr.type = htole32(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
	s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));

	s.req.r.x = htole32(x);
	s.req.r.y = htole32(y);
	s.req.r.width = htole32(width);
	s.req.r.height = htole32(height);

	s.req.offset = htole64((y * sc->vtgpu_fb_info.fb_width + x)
	 * (sc->vtgpu_fb_info.fb_bpp / 8));
	s.req.resource_id = htole32(VTGPU_RESOURCE_ID);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);

	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA)) {
		device_printf(sc->vtgpu_dev, "Invalid reponse type %x\n",
		    le32toh(s.resp.type));
		return (EINVAL);
	}

	return (0);
}

static int
vtgpu_resource_flush(struct vtgpu_softc *sc, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
	struct {
		struct virtio_gpu_resource_flush req;
		char pad;
		struct virtio_gpu_ctrl_hdr resp;
	} s = { 0 };
	int error;

	s.req.hdr.type = htole32(VIRTIO_GPU_CMD_RESOURCE_FLUSH);
	s.req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
	s.req.hdr.fence_id = htole64(
	    atomic_fetchadd_64(&sc->vtgpu_next_fence, 1));

	s.req.r.x = htole32(x);
	s.req.r.y = htole32(y);
	s.req.r.width = htole32(width);
	s.req.r.height = htole32(height);

	s.req.resource_id = htole32(VTGPU_RESOURCE_ID);

	error = vtgpu_req_resp(sc, &s.req, sizeof(s.req), &s.resp,
	    sizeof(s.resp));
	if (error != 0)
		return (error);

	if (s.resp.type != htole32(VIRTIO_GPU_RESP_OK_NODATA)) {
		device_printf(sc->vtgpu_dev, "Invalid reponse type %x\n",
		    le32toh(s.resp.type));
		return (EINVAL);
	}

	return (0);
}
