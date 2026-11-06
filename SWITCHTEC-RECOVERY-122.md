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

## Recovery status

Host-wide reboot requested from the user because it exceeds the targeted-port
test and is needed to clear the unkillable driver thread and reinitialize the
switch. Existing GRUB default verified as `7.0.0-rebar-aer-bmc`; no new boot
target or kernel was installed. Recovery is pending approval/verification.

Evidence directory on the host: `/home/ubuntu/hotplug-recovery-20261106/`.
Includes before/after kernel logs, port snapshots, binding information, the
test transcript, persistence stack and service-stop failure, and boot settings.
