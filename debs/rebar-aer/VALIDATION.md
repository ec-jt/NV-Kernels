# Package validation: 7.0.0-rebar-aer

Built on 2026-11-05 on the existing .107 build host from source commit
6a7d62a4d, using GCC 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1).

## Passed checks

- Full kernel/module Debian build exited 0; package validation exited 0.
- All three packages identify as amd64, version 7.0.0-2, with the expected
  package names. All archives extracted successfully.
- The extracted kernel configuration exactly matches the committed release
  configuration. AER, PCIe port services, AST, HID, generic HID, USB HID,
  BTF and IOMMUFD are enabled.
- The headers report kernelrelease 7.0.0-rebar-aer and include Module.symvers.
- Package maintainer scripts pass shell syntax checks.
- NVIDIA 615.78.08 nvidia, nvidia-uvm, nvidia-modeset and nvidia-drm compiled
  against the extracted headers. All four report version 615.78.08 and
  vermagic beginning with 7.0.0-rebar-aer.
- SHA256SUMS verification passed for all three Debian packages.

Packaged kernel image SHA-256:

```text
486d357464c1ef4374a1bf142e5c1277daf366e7f11860624bc33c4a3f80e1a1
```

## Validation boundary

The earlier 7.0.0-rebar-usb-bmc-test1 image was boot-tested with this
functional source and configuration, including a boot without pci=noaer,
eight valid 32 GiB GPU BAR1s and all-GPU GDS integrity checks. Only the
release-name configuration changes for this package build.

The renamed packaged image has not itself been installed or booted. The
NVIDIA modules compiled during package validation were not installed or
loaded and are not bundled in these Debian packages. Production deployment
still requires matching third-party modules and normal boot validation.

The native AER test did not establish GPU-facing root-port AER support or
GPU fatal-error recovery; see AER-TEST.md. No error injection was performed.

## Build records

The release directory on .107 is
/home/ubuntu/rebar-aer-release-20261105. It contains package-build.log,
package-build.exit, validation.log, validation.exit, the extracted package
contents and NVIDIA header-build log under validation/.

The initial dependency check failed because libdw-dev was missing; it was
resolved before the successful build. Stale package metadata also referenced
an unavailable liblzma-dev version. Refreshing metadata and installing the
matching 5.6.1+really5.4.5-1ubuntu0.4 runtime/development pair resolved this.
The unrelated old kernel/DKMS configuration was not rerun.

The running test kernel, installed NVIDIA driver and fallback GRUB selection
were not changed by the release build.
