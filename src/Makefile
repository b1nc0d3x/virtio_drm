# SPDX-License-Identifier: BSD-2-Clause
#
# Out-of-tree kernel module for the virtio_drm driver.
#
# Cross-build on a FreeBSD/amd64 build host for arm64:
#
#   make TARGET_ARCH=aarch64
#
# Native build on the target:
#
#   make
#
# Produces virtio_drm.ko in the current directory.

KMOD=	virtio_drm
SRCS=	virtio_drm.c
SRCS+=	bus_if.h device_if.h virtio_bus_if.h virtio_if.h fb_if.h
SRCS+=	opt_syscons.h opt_platform.h opt_acpi.h opt_teken.h opt_kbd.h opt_evdev.h opt_splash.h

# Default to native arch unless caller overrides TARGET_ARCH.
.if defined(TARGET_ARCH)
MACHINE_ARCH=	${TARGET_ARCH}
.endif

# Resolve src tree for the cross-build sysroot.
SYSDIR?=	/usr/src/sys

CFLAGS+=	-I${SYSDIR}

.include <bsd.kmod.mk>
