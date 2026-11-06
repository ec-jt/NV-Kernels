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

## BMC chassis cycle recovered GPU 5

The user provided access to the host's BMC. Cleanly unloaded nvidia-fs, UVM,
and NVIDIA and shut down the OS. Redfish confirmed chassis PowerState `Off`;
after more than one minute off, the BMC accepted an `On` action. SSH returned
around 13:04 UTC on the same one-time `7.0.0-hotplug` entry. The persistent
old-kernel default remains unchanged.

GPU 5 now enumerates at `89:00.0`, UUID
`GPU-af00d5c3-2e76-0a37-f1b9-b01b16f7d691`, with a Gen4 x16 link. Before
loading NVIDIA, its logical slot cycle passed in 3.82 s with neighbour links
and BARs unchanged. All eight GPUs passed the 32 GiB BAR1/ancestor-window
validator. GPU 5 then passed 127 CUDA pattern-check iterations over 15 s.

**This was a chassis power cycle, not a verified removal of all GPU power.**
The user clarified that four external HP 1500 W PSUs supply auxiliary power
to the eight GPUs (6000 W nominal aggregate nameplate capacity).
The BMC's one healthy chassis PSU and second Critical/Disabled PSU with 0 V
input do not account for those external supplies. No external PSU was switched
off, and no per-GPU power rail was measured. The observed recovery does not
identify a faulty connector, PSU, GPU, riser, or switch, nor prove the prior
intermittent failure is repaired.

## Recurrent fault and recovery without reboot

All eight bound-removal guards passed with GPU 4 running CUDA. During the
subsequent GDS run, GPU 0 and GPU 1 completed verified NVFS reads. The GPU 2
probe then aborted in `CUFileThreadPoolWorker::run`. At boot time 197.728965 s,
the kernel logged Xid 79 for GPU 5 (`89:00.0`, process `gds_probe`). The switch
again reported its port DOWN in `Polling (COMP)` and config space read all
ones. This occurred approximately three minutes after the chassis boot.

At 198.984751 s, pciehp logged that the non-HPC slot event was deferred while
the GPU driver remained bound. At 204.117010 s, UVM reported global fatal
error 0x60. Other GPUs acquired the driver-level OS-reboot recovery flag;
their physical links and the switch management endpoint remained accessible.
The eight-GPU GDS run therefore **failed**, and no stability claim is made.

With the failed probe gone and no GPU clients open, `modprobe -r nvidia_fs
nvidia_uvm nvidia` completed successfully. Modeset/DRM were absent. The
guarded pciehp slot interface then removed the unbound, failed GPU and its
audio function. A single enable attempt returned ENODEV because the physical
link remained down. All seven peer BARs, the neighbour's x16 link, and switch
management were preserved; no firmware port-unbind or bridge reset was used.

Reloading the compute modules restored the seven healthy UUIDs. Every one
passed CUDA pattern checks and synchronous NVFS reads with data verification
and zero POSIX fallback, without another OS reboot. This validates one
manual, compute-only fault-containment/reinitialization path after all users
have stopped. It does not preserve CUDA contexts or running distributed jobs,
and it does not bring the failed card back.

No Switchtec firmware unbind, whole-switch reset, or undocumented GPIO write
should be repeated. Physical powered replacement and automatic recovery from
an in-use GPU loss remain unvalidated. The recurrent endpoint/link failure
still needs investigation independently of the hotplug kernel.

Host remains in the maintenance boot with matching compute modules loaded,
seven responding GPUs capped at 400 W, GPU 5 removed from the PCI device list,
and GPU jobs/startup services held off. Original
autoload files are retained under `temporarily-disabled-autoload/`; original
configuration backups are in the evidence directory. Restore deliberately
after recovery testing, rather than restarting GPU workloads on the missing
card's old index. GPU indices have changed; use UUID or PCI BDF.

Evidence directory on the host: `/home/ubuntu/hotplug-recovery-20261106/`.
Includes before/after kernel logs, port snapshots, binding information, the
test transcript, persistence stack and service-stop failure, and boot settings.
