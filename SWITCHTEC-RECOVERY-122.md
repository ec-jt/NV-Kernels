# Failed-endpoint recovery trial, 2026-11-06

Host: server22 (`10.101.8.122`). Running kernel: `7.0.0-rebar-aer-bmc #3`.
This trial did not install or boot the new hotplug kernel. It tested firmware
recovery of an endpoint that had already fallen off the bus.

## Initial state

- GPU 5: `0000:89:00.0`, behind `0000:87:01.0`, Switchtec2 physical port 32,
  logical port 2, partition 0. Config-space vendor/device read `ffffffff`;
  link width x0, DLL inactive, switch LTSSM `Polling (COMP)`.
- Neighbour GPU 4: `0000:88:00.0`, behind `0000:87:00.0`, physical port 24,
  logical port 1. Link up x16. Switch uplink physical port 8 was up Gen4 x16.
- Switch: PFX 0x4052, REVB, firmware 3.50 B03E.
- No compute jobs were observed. The persistence daemon held GPU handles.

## Driver drain failure

Temporarily stopped persistence, GPU exporter, CDI path and the read-only
monitoring agent to avoid new GPU clients. Persistence PID 97556 became stuck
in `nvkms_yield -> nvEvoMakeRoom` in `nvidia_modeset` while closing handles.
Systemd's SIGKILL did not terminate it. GPU handles remained open; the test
therefore refused to unload NVIDIA. No forced module unload was attempted.

This is a second failure mode beyond the `nv_pci_remove_helper` usage-count
wait seen on the healthy test host: even closing a client can spin after the
endpoint has failed. Both need resolution for reliable live fault recovery.

## Firmware port-unbind failure

At approximately 12:32 UTC, attempted only:

```text
switchtec port-unbind /dev/switchtec2 -p 0 -l 2
```

Preflight verified that GPU 5 was already inaccessible, no pciehp handler was
attached to that port, only the known stuck persistence process held GPU
handles, and the peer/uplink were still connected. This was an explicit
firmware-only fault-recovery experiment, not a successful drained hotplug test.

The command returned I/O error. The subsequent binding query also returned
I/O error. The switch management endpoint, upstream switch function and both
downstream ports then read as inaccessible. GPU 4 logged Xid 79 at 12:32:32.
The intended rebind could not be performed through the inaccessible endpoint.
No repeated unbind, whole-switch hard reset, GPIO write, or firmware update
was issued.

The CPU root port `0000:80:03.1` still reported Gen4 x16 and DLL active, with
DPC not triggered. SSH and root NVMe remained accessible. The exact cause
inside switch firmware/routing is not established; these observations do not
prove that the switch lost electrical power.

**Do not treat port unbind/rebind as an isolated GPU recovery procedure on
this firmware.** It affected the neighbouring GPU and made management
unavailable. No new run of this experiment should happen without an out-of-band
recovery path and all devices behind the switch drained.

## Authorized recovery boot and hotplug validation

The user subsequently authorized rebooting and changing the kernel. Installed
image and headers `7.0.0-2+hotplug`, with a one-time GRUB entry selecting
`7.0.0-hotplug`. The saved default remains `7.0.0-rebar-aer-bmc`. A synchronized,
read-only-filesystem emergency reboot cleared the stuck modeset thread. SSH
returned at approximately 12:46 UTC on 2026-11-06.

The matching NVIDIA 615.78.08 and nvidia-fs 2.31.5 modules were copied from the
validated `.107` build for the identical kernel. Their SHA256 hashes matched.
615.78.08 user-space libraries were already installed; the NVIDIA runfile was
not rerun. `/home/ubuntu/open-gpu-kernel-modules` on this host still contains
610.43.02 source and local changes; it was preserved, not rebuilt over 615.

Eight known GPU-facing ports were opted into polling pciehp:
`03:00.0`, `03:01.0`, `42:00.0`, `42:01.0`, `87:00.0`, `87:01.0`,
`c2:00.0`, and `c2:01.0`, all in domain 0000.

The switch management endpoint and GPU 4 recovered. GPU 5 (`89:00.0`) did
not enumerate. Its physical port 32 remains bound to logical port 2 but stays
DOWN in `Polling (COMP)`. A warm reboot therefore did not restore its link.
This does not identify which component has failed or prove loss of 12 V.

NVIDIA modeset/DRM and GPU background services were held off. An additional
Docker startup hook (`10-wait-gpu.conf`) repeatedly invoked `nvidia-smi` and
autoloaded the compute modules despite the alias blacklist. Docker, its socket,
and the decider updater were temporarily runtime-masked for the test. After
removing those clients, `nvidia_uvm` and `nvidia` unloaded cleanly.

Completed checks:

| Test | Result |
| --- | --- |
| Quiesced logical remove/add, all seven responding GPUs | Passed; 2.29–4.03 s per cycle; all GPU BAR allocations and neighbour link widths/active states unchanged |
| FLR of GPU 4 (`88:00.0`), NVIDIA unloaded | Passed, 0.112 s; BAR resources unchanged |
| ReBAR allocation and ancestor forwarding | Passed on seven GPUs, each with 32 GiB BAR1 |
| Bound-device removal guard | All seven rejected immediately with EBUSY; concurrent GPU 4 CUDA remained correct |
| CUDA after module reload | Seven GPUs passed 32 MiB pattern/write/read checks; 26 iterations each over 3 s |
| GDS after module reload | Seven GPUs passed synchronous NVFS reads with data verification and zero POSIX fallback |
| Networking | Management Ethernet up; ConnectX-5 InfiniBand Active, LinkUp, 100 Gb/s with in-tree mlx5 |
| IB headers metadata | `drivers/infiniband/core/Makefile` present in installed headers |

An initial FLR test's cleanup rejected restoring the cached `flr bus` reset
method list because the kernel no longer considered `bus` supported. The test
was corrected to re-probe supported methods on EINVAL; the completed FLR and
subsequent CUDA/GDS checks passed. No bridge reset was used.

The OFED 25.10 external modules still have separate Linux 7 API incompatibilities.
Candidate-specific DKMS exclusions were recorded under `/etc/dkms`; the inbox
mlx5/IB stack is in use. `openibd.service` reports failure and must not be
mistaken for a validated OFED installation.

The successful headless tests do not fix NVIDIA's surprise-loss teardown. The
615.78.08 modeset source still contains the unbounded `nvEvoMakeRoom` wait
observed before the reboot. A simple timeout return would violate callers'
assumption that push-buffer space is available; proper lost-device/error
propagation is separate driver work. No driver-source patch was installed here.

Remaining: obtain BMC access for a cold power-off/on trial of GPU 5, then
physical inspection if its link stays down. No Switchtec firmware unbind,
whole-switch reset, or undocumented GPIO write should be repeated. Physical
powered replacement and automatic recovery from an in-use GPU loss remain
unvalidated.

Host left in the maintenance boot with compute modules and nvidia-fs loaded,
seven GPUs capped at 400 W, and GPU jobs/startup services held off. Original
autoload files are retained under `temporarily-disabled-autoload/`; original
configuration backups are in the evidence directory. Restore deliberately
after recovery testing, rather than restarting GPU workloads on the missing
card's old index. GPU indices have changed; use UUID or PCI BDF.

Evidence directory on the host: `/home/ubuntu/hotplug-recovery-20261106/`.
Includes before/after kernel logs, port snapshots, binding information, the
test transcript, persistence stack and service-stop failure, and boot settings.
