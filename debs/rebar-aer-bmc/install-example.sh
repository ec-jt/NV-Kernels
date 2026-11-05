#!/bin/bash
set -e

base="https://github.com/ec-jt/NV-Kernels/releases/download/rebar-aer-bmc-7.0.0"

wget -c "$base/linux-headers-7.0.0-rebar-aer-bmc_7.0.0-3_amd64.deb"
wget -c "$base/linux-image-7.0.0-rebar-aer-bmc_7.0.0-3_amd64.deb"
wget -c "$base/linux-libc-dev_7.0.0-3_amd64.deb"
wget -O SHA256SUMS "$base/SHA256SUMS"
sha256sum --ignore-missing -c SHA256SUMS

dpkg -i linux-headers-7.0.0-rebar-aer-bmc_7.0.0-3_amd64.deb \
        linux-image-7.0.0-rebar-aer-bmc_7.0.0-3_amd64.deb \
        linux-libc-dev_7.0.0-3_amd64.deb

cp -a /etc/default/grub /etc/default/grub.before-rebar-aer-bmc
sed -i 's/^GRUB_CMDLINE_LINUX=.*/GRUB_CMDLINE_LINUX="pcie_aspm=off iommu=pt iommu.strict=0 pcie_port_pm=off pci=realloc=on pcie_ports=native log_buf_len=64M loglevel=7 nvme_core.default_ps_max_latency_us=0 printk.devkmsg=on video=efifb:off"/' /etc/default/grub
update-grub

# Ensure other GRUB variables/drop-ins do not still add pci=noaer.
# Rebuild/install NVIDIA modules for 7.0.0-rebar-aer-bmc before using its GPUs.
# Existing custom GRUB defaults may still select an older kernel.
