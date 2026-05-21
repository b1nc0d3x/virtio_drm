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

## Why this driver exists

`drm-kmod` (the FreeBSD port of the Linux DRM-KMS stack via the
`LinuxKPI` shim) is **amd64-only**.  LinuxKPI has never been
ported to aarch64, and the porting effort is enormous.  As a
result, every FreeBSD-arm64 VM today is stuck on `efifb`:

  - a static framebuffer at whatever resolution UEFI happened
    to set,
  - no modeset, no DPMS, no GPU acceleration,
  - no `/dev/dri/card0`, so no X11 modesetting driver, no
    Wayland compositor,
  - no dynamic resize when the QEMU/UTM window changes,
  - no second-monitor support.

`virtio-gpu` is the paravirtual display device exposed by every
hypervisor that matters for arm64: QEMU/KVM, Apple's
`Virtualization.framework` (UTM, vagrant-apple-vz), Cloud
Hypervisor, Firecracker.  A working DRM-KMS driver for it
single-handedly closes the arm64-VM graphics gap and validates
the methodology for non-LinuxKPI DRM drivers on FreeBSD.

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

## Tutorial — try it in a QEMU VM

This section walks through standing up a FreeBSD/arm64 VM under
QEMU and bringing up `virtio_drm` against the paravirtual
`virtio-gpu` device.  Tested on a FreeBSD/amd64 build host with
qemu 10.x.

### 1. Install QEMU and grab the firmware

On the build host:

```sh
sudo pkg install qemu
ls /usr/local/share/qemu/edk2-aarch64-code.fd   # UEFI firmware blob
```

### 2. Get a FreeBSD/arm64 disk image

```sh
mkdir -p ~/vm/virtio_drm && cd ~/vm/virtio_drm
fetch https://download.freebsd.org/releases/VM-IMAGES/15.0-RELEASE/aarch64/Latest/FreeBSD-15.0-RELEASE-arm64-aarch64-ufs.raw.xz
unxz FreeBSD-15.0-RELEASE-arm64-aarch64-ufs.raw.xz
```

Copy the UEFI firmware in and create a 64 MB writable nvram
backing file (UEFI variables persist between boots into this
file):

```sh
cp /usr/local/share/qemu/edk2-aarch64-code.fd ./edk2-aarch64-code.fd
dd if=/dev/zero of=./edk2-arm-vars.fd bs=1M count=64
```

### 3. Pre-configure the disk image (offline)

The stock VM image has no SSH access set up.  Mount it on the
host, add the SSH key, and drop the kernel module into
`/boot/modules`:

```sh
sudo mdconfig -a -t vnode -f FreeBSD-15.0-RELEASE-arm64-aarch64-ufs.raw -u 99
sudo mount /dev/md99p3 /mnt

# Enable sshd + DHCP
sudo sh -c 'cat >> /mnt/etc/rc.conf <<EOF
sshd_enable="YES"
ifconfig_DEFAULT="DHCP"
hostname="vdrmtest"
EOF'
sudo sed -i "" 's/^#PermitRootLogin .*/PermitRootLogin yes/' /mnt/etc/ssh/sshd_config
sudo mkdir -p /mnt/root/.ssh
sudo cp ~/.ssh/id_ed25519.pub /mnt/root/.ssh/authorized_keys
sudo chmod 700 /mnt/root/.ssh
sudo chmod 600 /mnt/root/.ssh/authorized_keys

# Install the OOT module and preload it so it wins the boot probe
sudo cp /path/to/virtio_drm/src/virtio_drm.ko /mnt/boot/modules/
sudo sh -c 'cat > /mnt/boot/loader.conf <<EOF
virtio_drm_load="YES"
EOF'

sudo umount /mnt
sudo mdconfig -d -u 99
```

`virtio_drm_load="YES"` tells the loader to pull our module
into memory before the kernel starts probing devices.  Our
`vtgpu_probe()` returns `BUS_PROBE_VENDOR` (higher priority
than the in-base `virtio_gpu` driver's `BUS_PROBE_DEFAULT`),
so newbus picks `virtio_drm` to bind the VirtIO GPU device.

### 4. Launch QEMU

A minimal launch script.  Save as `run.sh`:

```sh
#!/bin/sh
set -eu
cd "$(dirname "$0")"
exec qemu-system-aarch64 \
    -name virtio_drm-test \
    -machine virt,gic-version=3 \
    -cpu cortex-a72 \
    -smp 4 \
    -m 4G \
    -nodefaults \
    -drive if=pflash,format=raw,readonly=on,file=edk2-aarch64-code.fd \
    -drive if=pflash,format=raw,file=edk2-arm-vars.fd \
    -drive if=none,id=drive0,format=raw,file=FreeBSD-15.0-RELEASE-arm64-aarch64-ufs.raw \
    -device virtio-blk-pci,drive=drive0,bootindex=1 \
    -device virtio-gpu-pci \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-tablet,bus=xhci.0 \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-rng-pci \
    -display gtk \
    -serial file:/tmp/qemu.serial.log
```

```sh
chmod +x run.sh
./run.sh &
```

A GTK window appears.  Pure-headless?  Swap `-display gtk` for
`-display vnc=:0` and connect a VNC client to `host:5900`.

### 5. Verify the driver attached from inside the VM

The module preloads at boot, so by the time you log in it has
already bound the device.

```sh
ssh -p 2222 root@127.0.0.1
# ... in the VM ...
kldstat | grep virtio_drm
dmesg | grep -iE "vgpu|virtio_drm"
# Expected output:
#   vgpu0: <VirtIO GPU (virtio_drm)> on virtio_pci1
#   vgpu0: scanout 0: 640x480+0+0 flags=0x0
#   vgpu0: scanout 0: EDID 1024 bytes
#   VT: initialize with new VT driver "virtio_drm".

vmstat -i | grep virtio_pci  # IRQ rate on the GPU vq
```

### 6. Iterate

After editing the source, rebuild on the host, scp the new `.ko`
into the running VM, and reload:

```sh
# on the host
cd /path/to/virtio_drm/src && make TARGET_ARCH=aarch64
scp -P 2222 virtio_drm.ko root@127.0.0.1:/boot/modules/

# in the VM
kldunload virtio_drm
kldload virtio_drm
```

If `kldunload` fails because the device is still attached, do
`devctl detach <unit>` first — but note that on arm64 GENERIC a
clean detach requires our 1.C teardown code to have run, so
freshly-loaded modules are usually safe to unload.

### Known gotchas

  - **The in-base `virtio_gpu` is statically linked into arm64
    `GENERIC`.**  Our higher `BUS_PROBE_VENDOR` priority is what
    lets the OOT module win the probe.  If you find a kernel
    config that has the in-base driver compiled out, that works
    too — just `kldload` after boot instead of preloading.
  - **Custom `/usr/src/sys/conf/ldscript.kmod.arm64` will break
    preloading.**  If your `/usr/src` is a development tree that
    has added a per-arch kmod linker script, you'll see a kernel
    panic in `preload_protect` at uptime 1s when loading any
    cross-built module.  Symptom: `panic: vm_fault failed: <PC>
    error 1` very early in boot, before any driver attach.
    Stock linker behaviour (no custom script) produces a
    PHDR-bearing layout with a read-only LOAD segment first;
    that's what the kernel's preload path expects.  Move the
    file aside and rebuild.
  - **`devctl detach vtgpu0; kldload virtio_drm` panics on
    arm64.**  The upstream in-base `vtgpu_detach` doesn't tell
    the host to release its sglist (this fork's
    `virtio_drm_detach` is fixed, but only for *our* driver —
    not retroactively for the in-base one).  Use the preload
    path instead.
  - **`-display gtk` requires `$DISPLAY` on the build host.**  On
    a headless server, prefer VNC.

## Status

  - **Phase 0:**  fork compiles as `virtio_drm.ko` for arm64.
  - **Phase 1:**  `RESOURCE_UNREF` / `DETACH_BACKING` lifecycle,
    runtime `GET_EDID`, multi-scanout enumeration,
    `VIRTIO_GPU_EVENT_DISPLAY` hotplug listener.  Still an fbio
    framebuffer; the driver now correctly mirrors what the host
    advertises.
  - **Phase 2 (next):**  cursor (UPDATE_CURSOR, MOVE_CURSOR) on
    the cursor virtqueue.
  - **Phase 3:**  graft DRM-KMS frontend on `drm2` —
    `/dev/dri/card0`, X11 modesetting driver.
  - **Phase 4+:** blob resources, render node, eventually
    VirGL/Venus.

## License

BSD-2-Clause.  See header in each source file for the chain of
copyright (Venteicher 2013, Arm Ltd 2023, Crenshaw 2026).
