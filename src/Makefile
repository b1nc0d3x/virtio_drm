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
SRCS+=	bus_if.h device_if.h virtio_bus_if.h virtio_if.h fb_if.h pci_if.h
SRCS+=	opt_syscons.h opt_platform.h opt_acpi.h opt_teken.h opt_kbd.h opt_evdev.h opt_splash.h opt_drm.h opt_vm.h

# Default to native arch unless caller overrides TARGET_ARCH.
.if defined(TARGET_ARCH)
MACHINE_ARCH=	${TARGET_ARCH}
.endif

# Resolve src tree for the cross-build sysroot.
SYSDIR?=	/usr/src/sys

CFLAGS+=	-I${SYSDIR}

# NOTE on /boot/loader.conf preload:
# Loading virtio_drm.ko via virtio_drm_load="YES" in /boot/loader.conf
# currently panics on FreeBSD-arm64 inside preload_protect (link_elf.c
# reads ef->address as Elf_Ehdr and gets garbage).  Root cause is
# upstream — the arm64 EFI loader's elf64_loadimage sets fp->f_addr
# (i.e. MODINFO_ADDR) such that the kernel cannot find the ELF header
# at that address.  We tried forcing FILEHDR + PHDRS into PT_LOAD[0]
# via a custom ldscript (ldscript.kmod.virtio_drm in this dir) and the
# resulting .ko has the header in PT_LOAD[0] file_offset=0 vaddr=0 —
# but the kernel still reads .text-like bytes from ef->address+0x20.
# Bug filed for stand/efi/loader/arch/arm64 follow-up.
#
# Workaround: load at runtime via /etc/rc.conf kld_list="virtio_drm".
# Adds ~1s to boot but is otherwise functionally equivalent.

.include <bsd.kmod.mk>
