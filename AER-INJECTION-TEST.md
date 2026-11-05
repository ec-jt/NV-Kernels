# Correctable AER software-injection test

Test date: 2026-11-05. Test host: eight RTX 5090s on GIGABYTE MZ22-G20-00.
Diagnostic kernel: `7.0.0-rebar-aer-inject-test1`; NVIDIA driver: 615.78.08.

The diagnostic configuration adds `CONFIG_PCIEAER_INJECT=m` and
`CONFIG_GENERIC_IRQ_INJECTION=y` to the earlier validated USB/BMC configuration.
The `7.0.0-rebar-aer-bmc` release uses this tested configuration with only
CONFIG_LOCALVERSION changed.

## Result

At 16:10:04 UTC, the unmodified upstream aer_inject module injected one
correctable receiver error into internal AMD root port `0000:00:07.1`.
Error-mask override was disabled. No uncorrectable or fatal error was requested.

| Counter | Before | After |
| --- | ---: | ---: |
| Root correctable | 0 | 1 |
| Root nonfatal | 0 | 0 |
| Root fatal | 0 | 0 |
| Device RxErr | 0 | 1 |

The kernel logged the correctable error, its source BDF, and RxErr bit.
The injector was then unloaded; its device node disappeared and normal
PCI configuration-access operations were restored.

All eight GPUs remained visible and retained valid 32 GiB BAR1 allocations.
Post-test GDS integrity checks passed on every GPU with zero POSIX fallback.
No NVIDIA Xid, uncorrectable AER, API mismatch or unknown-symbol messages
were found in the checked log.

## What this establishes

The Linux interrupt, reporting and counter path handles a software-injected
correctable error on the tested internal root port. The normal Physical Layer
label in the message does not mean an actual electrical fault was created.

This is not a GPU error-delivery or fatal-error-recovery test. The four
GPU-facing root ports still expose no AER capability. AER does not force
x16 link width or fix signal integrity. ReBAR mapping and link training
remain separate mechanisms.

The injector is an optional module and is not loaded by default. Loading
it and writing an event are explicit privileged diagnostic operations.

## Evidence

On .107, the full test scripts and logs are under
`/home/ubuntu/rebar-aer-inject-20261105/`, including
`correctable-test/`, `post-injection-rebar.txt`,
`post-injection-gds.log`, `post-injection-kernel.log` and `RESULT.txt`.

The test kernel's one-time boot entry was consumed. The normal fallback was
not changed. See [AER-TEST.md](AER-TEST.md) for the earlier AER boot test.
