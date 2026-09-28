# Mixed RTX ReBAR kernel packages

Kernel release: 7.0.0-rebar-mixed-5070ti-5090.
Debian package version: 7.0.0-1.
Architecture: amd64.
Source branch: rebar-mixed-5070ti-5090.

The functional change is committed as b2822a76e708. Subsequent annotation
cleanup changes comments and documentation only. The tested configuration is
[`configs/config-7.0.0-rebar-mixed-5070ti-5090`](../../configs/config-7.0.0-rebar-mixed-5070ti-5090).
See [REBAR-MIXED-TOPOLOGY.md](../../REBAR-MIXED-TOPOLOGY.md) for implementation,
hardware validation, boot configuration, and recovery details.

## Artifacts

| Package | Purpose |
| --- | --- |
| [Image](linux-image-7.0.0-rebar-mixed-5070ti-5090_7.0.0-1_amd64.deb) | Kernel image and in-tree modules |
| [Headers](linux-headers-7.0.0-rebar-mixed-5070ti-5090_7.0.0-1_amd64.deb) | External-module and NVIDIA builds |
| [Userspace headers](linux-libc-dev_7.0.0-1_amd64.deb) | Optional UAPI development headers |

Integrity metadata is in [SHA256SUMS](SHA256SUMS),
[build information](linux-upstream_7.0.0-1_amd64.buildinfo), and
[Debian changes](linux-upstream_7.0.0-1_amd64.changes).

```sh
sha256sum -c SHA256SUMS
sudo dpkg -i linux-headers-7.0.0-rebar-mixed-5070ti-5090_7.0.0-1_amd64.deb \
    linux-image-7.0.0-rebar-mixed-5070ti-5090_7.0.0-1_amd64.deb
```

Installing the userspace-header package is not necessary to boot this kernel.
The image package does not include separately built NVIDIA modules. Rebuild
NVIDIA for this release and verify any third-party DKMS results. Do not mask
package-installation failures or remove known-good fallback kernels.

Use a single effective PCI reallocation enable setting. Remove any later
disable option inherited from another GRUB variable or drop-in before booting.

```text
pci=realloc=on
```

## Build and validation scope

The full source build was boot-tested on the mixed eight-5070-Ti/four-5090
server. All twelve devices passed BAR allocation and ancestor-window checks
and an 8 MiB CUDA memory fill/read-back smoke test. NVIDIA independently
reported eight 16 GiB and four 32 GiB BAR1 apertures. Switchtec management,
the AST console driver, NVMe root storage, and management networking remained
available.

These packages were produced by the subsequent Debian packaging rebuild using
the same functional source, identical kernel configuration, release, build
version, timestamp, and compiler. The packaged kernel retains BTF data. The
standalone debug-symbol package was omitted using the packaging profile;
BTF was not disabled.

The packaging rebuild is not byte-identical to the installed, boot-tested
image. These are not claimed to be reproducible binary builds, and the
packaging rebuild itself has not received a separate boot test. The validated
image remains installed and selected as the default on the test server.

Image hashes, distinct from the package hashes in [SHA256SUMS](SHA256SUMS):

```text
Boot-tested image:
a7881b1a1d9cba85ae01a4fb8d150619d1556735d61a752ca7bf49767b9edbf4

Debian packaging rebuild image:
22513c47414d86f1f3e81f975515ac4444a7f86cafe71a23c454e3507ac00ae3
```

The legacy USB/SATA NP-relief quirks are still present. Review the source guide
before deploying on a system that needs SATA root storage or USB controllers.
Peer-to-peer and RDMA stress testing are outside the completed validation.
