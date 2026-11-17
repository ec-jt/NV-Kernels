#!/bin/bash
# Install the barfix1 test release. Does not reboot or choose a GRUB entry.
set -euo pipefail
[[ $(id -u) == 0 ]] || { echo 'Run with sudo.' >&2; exit 1; }
base=https://github.com/ec-jt/NV-Kernels/releases/download/hotplug-barfix1-7.0.0
release=7.0.0-hotplug-barfix1
version=7.0.0-1+barfix1
work=$(mktemp -d /var/tmp/nv-kernels-barfix1.XXXXXXXX)
cd "$work"
command -v wget >/dev/null
test -x /usr/bin/setpci
command -v update-initramfs >/dev/null
files=("linux-headers-${release}_${version}_amd64.deb"
       "linux-image-${release}_${version}_amd64.deb"
       lab-nvme-gen3-initramfs lab-nvme-gen3-initramfs-hook)
for file in "${files[@]}" SHA256SUMS; do
    wget --tries=3 --timeout=30 "$base/$file"
done
# Check exactly the files consumed by this installer; missing files are errors.
for file in "${files[@]}"; do
    awk -v f="$file" '$2 == f { print; found=1 } END { if (!found) exit 1 }' SHA256SUMS |
        sha256sum -c -
done
backup="/var/backups/nv-kernels-barfix1-$(date -u +%Y%m%dT%H%M%SZ)-$$"
install -d -m 700 "$backup"
for file in /etc/initramfs-tools/scripts/init-top/00-lab-nvme-gen3 \
            /etc/initramfs-tools/hooks/lab-nvme-gen3-tools; do
    if [[ -e "$file" ]]; then cp -a --parents "$file" "$backup/"; fi
done
install -D -m 755 lab-nvme-gen3-initramfs /etc/initramfs-tools/scripts/init-top/00-lab-nvme-gen3
install -D -m 755 lab-nvme-gen3-initramfs-hook /etc/initramfs-tools/hooks/lab-nvme-gen3-tools
dpkg -i "linux-headers-${release}_${version}_amd64.deb" \
        "linux-image-${release}_${version}_amd64.deb"
update-initramfs -u -k "$release"
update-grub
printf 'Installed %s. Matching NVIDIA/NVFS modules must be built separately.\n' "$release"
printf 'NVMe helper defaults off; lab_nvme_gen3=1 opts in on supported hardware.\n'
printf 'Downloads: %s\nOriginal helper backup: %s\nNo reboot requested.\n' "$work" "$backup"
