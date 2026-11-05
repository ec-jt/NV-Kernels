# Package validation: 7.0.0-rebar-aer-bmc

Built on 2026-11-05 on .107 from source commit 68aabbd553.
Debian package version: 7.0.0-3. Architecture: amd64.

## Passed

- Full kernel/module Debian build and package validation both exited 0.
- All three archives extracted and passed package-name/version/architecture checks.
- Packaged configuration matches the committed release config exactly.
- Compared with the hardware-tested 7.0.0-rebar-aer-inject-test1 configuration,
  only CONFIG_LOCALVERSION differs.
- AER, PCIe port services, AST, generic USB HID, BTF and IOMMUFD are enabled.
- CONFIG_PCIEAER_INJECT=m and CONFIG_GENERIC_IRQ_INJECTION=y are present.
- The packaged aer_inject module has the correct release vermagic.
- Packaged headers report 7.0.0-rebar-aer-bmc and include Module.symvers.
- All four NVIDIA 615.78.08 modules compiled against the extracted headers
  and report the correct driver version and kernel vermagic.
- Maintainer scripts and the installation example passed shell syntax checks.
- SHA256SUMS verification passed for all three Debian packages.

Packaged kernel image SHA-256:

```text
e153cb025c2ce6b0faa646037ceab210ae03b39d6e6422bd9281846dafe1cf0a
```

## Hardware validation boundary

The same functional source/configuration was boot-tested under the diagnostic
release name. All eight 5090 GPUs retained valid 32 GiB BAR1 allocations.
One correctable receiver event on internal root port 0000:00:07.1 was logged
and counted (0 -> 1), while nonfatal/fatal counters stayed zero. The injector
was unloaded. Post-test all-GPU GDS integrity checks passed with zero POSIX
fallback. See ../../AER-INJECTION-TEST.md.

The renamed packaged image has not separately been installed or booted.
Validation-built NVIDIA modules were not installed or loaded and are not
bundled in these packages. The currently running diagnostic kernel and
fallback selection were not changed during this release build.

GPU-facing root-port AER and fatal-error recovery remain unvalidated.
AER does not lock link width to x16. The optional injector is not loaded by
default.

## Evidence

Build and validation logs are in /home/ubuntu/rebar-aer-bmc-release-20261105:
package-build.log, package-build.exit, validation.log, validation.exit,
and validation/nvidia-headers-build.log. Build metadata and checksums are
included beside this file and attached to the GitHub release.
