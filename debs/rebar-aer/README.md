# Linux 7.0.0 ReBAR / USB / AER packages

Kernel release: `7.0.0-rebar-aer`.
Debian package version: `7.0.0-2`.
Architecture: `amd64`.
Source branch: `rebar-aer`.
Release tag: `rebar-aer-7.0.0`.

This release retains the mixed RTX 5070 Ti / RTX 5090 ReBAR allocator,
preserves USB controllers instead of clearing their BARs, and keeps the
ASPEED BMC VGA and Switchtec management resources intact. AST and generic
USB HID are built in. AER, BTF and IOMMUFD remain enabled in the kernel.

The release configuration is
[config-7.0.0-rebar-aer](../../configs/config-7.0.0-rebar-aer).
Compared with the boot-tested USB/BMC kernel configuration, only
CONFIG_LOCALVERSION changes. AER was already compiled into the earlier
kernel; removing `pci=noaer` enabled it at boot.

## Packages

- `linux-image-7.0.0-rebar-aer_7.0.0-2_amd64.deb`: kernel and in-tree modules.
- `linux-headers-7.0.0-rebar-aer_7.0.0-2_amd64.deb`: matching external-module headers.
- `linux-libc-dev_7.0.0-2_amd64.deb`: optional userspace development headers.

Binary packages are attached to the GitHub release; source, configuration,
checksums and release documentation are committed to this branch.

## Replacement deployment commands

Run these commands in a new download directory:

```sh
set -eu
base=https://github.com/ec-jt/NV-Kernels/releases/download/rebar-aer-7.0.0

wget "$base/linux-headers-7.0.0-rebar-aer_7.0.0-2_amd64.deb"
wget "$base/linux-image-7.0.0-rebar-aer_7.0.0-2_amd64.deb"
wget "$base/linux-libc-dev_7.0.0-2_amd64.deb"
wget "$base/SHA256SUMS"
sha256sum --ignore-missing -c SHA256SUMS

sudo dpkg -i linux-headers-7.0.0-rebar-aer_7.0.0-2_amd64.deb \
             linux-image-7.0.0-rebar-aer_7.0.0-2_amd64.deb
# Optional: needed only for UAPI development, not booting or NVIDIA modules.
# sudo dpkg -i linux-libc-dev_7.0.0-2_amd64.deb
```

The image package does not contain NVIDIA's out-of-tree driver. Build and
install the matching NVIDIA modules for `7.0.0-rebar-aer`, regenerate that
kernel's initramfs, and review any DKMS errors before booting. An installer
using `uname -r` targets the currently running kernel; do not run it before
the reboot and assume it built for the new release. Keep the previous kernel
and its modules installed.

Use a single effective `pci=realloc=on` setting. For the tested native AER
configuration retain `pcie_ports=native` and remove `pci=noaer` from all
active GRUB command-line variables and drop-ins. Do not replace unrelated
root, console, storage or VFIO options merely to change these PCI flags.

```sh
grep -R -nE 'GRUB_CMDLINE|noaer|realloc|pcie_ports' \
    /etc/default/grub /etc/default/grub.d
sudo update-grub
# After selecting and booting the new kernel:
uname -r
cat /proc/cmdline
```

The expected release is `7.0.0-rebar-aer`. `CONFIG_PCIEAER=y` alone does
not override a `pci=noaer` boot argument. `pcie_ports=native` forces native
PCIe service ownership and should be a deliberate platform configuration.

## Build

Source build revision: `6a7d62a4d`. Documentation/metadata commits may follow.

```sh
mkdir -p ../build-rebar-aer
cp configs/config-7.0.0-rebar-aer ../build-rebar-aer/.config
make O=../build-rebar-aer LOCALVERSION= olddefconfig
DEB_BUILD_PROFILES=pkg.linux-upstream.nokerneldbg \
KDEB_COMPRESS=xz KDEB_PKGVERSION=7.0.0-2 KBUILD_BUILD_VERSION=1 \
LOCALVERSION= make O=../build-rebar-aer -j24 bindeb-pkg
```

The packaging profile omits the separate debug-symbol package. It does not
disable BTF. The Debian package build requires libdw-dev, libelf-dev and
their development dependencies.

## Validation and limits

The functional source was boot-tested as `7.0.0-rebar-usb-bmc-test1` on the
eight-RTX-5090 GIGABYTE host. USB controllers, BMC keyboard (user confirmed),
AST video, all eight 32 GiB BAR1 allocations, CUDA memory fill/readback,
and GDS byte-verified reads passed. A separate boot without `pci=noaer`
succeeded; nine internal AMD root ports bound to the native AER driver.
Postboot GDS reads passed on all eight GPUs with zero POSIX fallback.

GPU-facing root ports on this machine do not expose an AER capability.
This release does not claim GPU AER delivery or fatal-error recovery is
fixed, nor does it force hidden capability registers into existence.
See [AER-TEST.md](../../AER-TEST.md) and
[USB-BMC-CONSOLE.md](../../USB-BMC-CONSOLE.md).

The renamed package build requires its own artifact checks. It is not
byte-identical to the test image and has not yet received a separate boot.
Package inspection results will be recorded in VALIDATION.md.

The legacy AMD 1022:7901 SATA-disabling quirk remains unchanged. This is a
custom NVMe-root lab kernel; review that quirk before using SATA storage.
These are locally built unsigned packages, without a bundled NVIDIA driver.
