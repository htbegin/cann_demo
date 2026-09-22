# A2 embedded-RoCE bandwidth results, 2026-09-22

Both GNU C89 benchmarks were built and executed on NPU 0 of hulk-0 and
hulk-1. Increasing the registered region transferred by each GET from 16 MiB
to 256 MiB raised sustained bandwidth from about 21.86 to **22.76 GiB/s**.
This is about 99% of the requested 23 GiB/s and matches the existing verbs
READ result. **23 GiB/s was not reached.**

## Initial and optimized results

Payload direction: hulk-0 source -> hulk-1 target. Bandwidth counts each
payload byte once. The target times submission through final stream
completion. Setup, warmup, final verification, and cleanup are excluded.

| Transfer | Initial GiB/s | Optimized median GiB/s | Three optimized runs, GiB/s | Gain |
| --- | ---: | ---: | --- | ---: |
| HBM -> HBM | 21.863187 | 22.761577 | 22.761753, 22.761577, 22.761155 | 4.11% |
| Host DDR -> HBM | 21.852141 | 22.760576 | 22.760576, 22.760967, 22.760314 | 4.16% |

Initial settings: 1 MiB per descriptor, 16 descriptors/GET, window 16,
20 warmup calls, 10,000 measured calls, no CPU affinity. Each initial run
transferred 156.25 GiB in approximately 7.15 seconds.

Selected settings: 16 MiB per descriptor, 16 descriptors/GET, window 16,
20 warmup calls, 1,250 measured calls, CPUs 144-167 on each host. Each repeat
transferred 312.5 GiB in approximately 13.73 seconds. This uses a 256 MiB
registered working set per peer; the window does not multiply that allocation.
The C programs keep their smaller defaults; the documented commands and SSH
controller select these throughput settings explicitly.

All measured pairs completed with exit status 0 and final PASS on both
peers, including full final-region verification and registration cleanup.
The target was re-poisoned after warmup, before timing. The source pattern
and working set are reused; verification checks final contents, not a
different pattern after every GET. This is a sustained large-transfer result,
not a latency result or a measurement of small discontiguous application data.

## What improved throughput

The initial GET moved 16 MiB; the selected GET moves 256 MiB. HCCL's
`HcclOneSidedConn::BatchRead` posts a fence/notification after every GET.
More bytes per GET amortize that work and API/task submission. Increasing
descriptor count while keeping 1 MiB descriptors reached the same bandwidth,
so a larger contiguous descriptor is not the only way to obtain this gain.

The application still waits after every 16 GET calls, and HCCL still applies
its per-GET completion fence. No completion checks were removed. The host
test retains the registered host-DDR source; no intermediate HBM payload
buffer was introduced. There were no library, driver, NIC, or system-wide
configuration changes.

| Sweep | Descriptor MiB | Batch | Window | Affinity | GiB/s |
| --- | ---: | ---: | ---: | --- | ---: |
| HBM baseline | 1 | 16 | 16 | None | 21.863187 |
| HBM larger descriptors | 4 | 16 | 16 | None | 22.562896 |
| HBM selected region | 16 | 16 | 16 | None | 22.760507 |
| HBM still larger descriptors | 64 | 4 | 16 | None | 22.756890 |
| HBM more descriptors | 1 | 256 | 16 | None | 22.760718 |
| HBM wait after every GET | 16 | 16 | 1 | None | 22.673307 |
| Host baseline | 1 | 16 | 16 | None | 21.852141 |
| Host selected region | 16 | 16 | 16 | None | 22.751570 |
| Host local CPU placement | 16 | 16 | 16 | 144-167 | 22.759258 |

Every sweep transferred the same 156.25 GiB. CPU affinity had little effect
in this sweep; the difference is too small to establish a NUMA bottleneck.
The final repeats used the local CPU set for consistent placement. `taskset`
sets CPU affinity, not a strict memory-allocation policy. Other unfold modes,
multiple communicators/QPs, and NIC/driver changes were not tested.

Reverse direction (hulk-1 -> hulk-0), one 156.25 GiB run each with the selected
sizes, window, and affinity: **22.754682 GiB/s HBM -> HBM** and
**22.736867 GiB/s host DDR -> HBM**, both verified successfully.

## Comparison with 23 GiB/s and hccn_tool

Both embedded ports currently report 200,000 Mb/s. The optimized median is
about 24.439 GB/s or 195.51 Gb/s, roughly 97.8% of that raw line rate.
23 GiB/s means 197.57 Gb/s of payload before protocol overhead.

The pre-existing [hccn READ report](hccn-read-existing.txt), copied from
hulk-0's `/tmp/recv_ib_read_bw.txt`, reports 23,302.26 MiB/s = **22.756113
GiB/s**, with one RC QP, 1 MiB messages, 128 outstanding READs and MTU 4096.
It was not rerun during this experiment. Its `MB/sec` column means MiB/s,
as explained in [BANDWIDTH.md](../../BANDWIDTH.md#comparing-with-hccn_tool).

The optimized HCCL measurements match that observed throughput plateau;
they do not establish a universal hardware maximum. This existing perftest
report does not establish its buffers' physical placement, so it is not
independent validation of the host-DDR path.

## Environment, reproduction, and evidence

| Node | SSH | Host control IP | NPU 0 RoCE IP | Build directory |
| --- | --- | --- | --- | --- |
| hulk-0 | hulk@116.204.37.141 | 192.168.0.217 | 29.184.165.65 | /tmp/a2-roce-bandwidth-build.foZ9JJ |
| hulk-1 | hulk@116.204.96.45 | 192.168.0.63 | 29.184.110.95 | /tmp/a2-roce-bandwidth-build.Yelr5h |

Both are aarch64 A2 nodes with 910B4 NPUs, installed CANN 8.5.2 and driver
25.5.1. NPU 0 is PCI device `0000:c1:00.0`, PCIe 16.0 GT/s x16, NUMA node 6,
local CPUs 144-167. All four demo executables compiled with the Makefile's
GNU C89 warnings-as-errors flags on both hosts. All bandwidth tests set:

```sh
HCCL_OP_EXPANSION_MODE=HOST
ASCEND_GLOBAL_LOG_LEVEL=3
ASCEND_SLOG_PRINT_TO_STDOUT=0
```

The initial device check found no NPU processes. A post-test check found
unrelated `VLLM::Worker_TP` processes on hulk-1 NPU 0 and NPU 1. Their start
times, 02:43:49 and 02:44:10 UTC, overlap the later sweep and final repeats.
Their activity during each timed interval was not monitored, so those runs
must not be described as fully isolated. They were not stopped or modified.
The three final repeats remained consistent despite this limitation. See
[process start times](hulk1-other-processes.txt) and
[post-test NPU state](hulk1-after.txt). No benchmark process remained in the
post-test NPU listings.

See [hulk-0 environment](hulk0-environment.txt) and
[hulk-1 environment](hulk1-environment.txt) for link speed, NUMA node, local
CPU list, PCIe speed/width, and SHA-256 hashes of the tested binaries, in
that order. [source-sha256.txt](source-sha256.txt) identifies the C/build inputs.

From the `demo` directory, while the isolated remote builds still exist:

```sh
python3 tools/run_hulk_bandwidth.py --kind hbm --name repeat-hbm \
  --hulk0-dir /tmp/a2-roce-bandwidth-build.foZ9JJ \
  --hulk1-dir /tmp/a2-roce-bandwidth-build.Yelr5h --out results/new-run
python3 tools/run_hulk_bandwidth.py --kind host --name repeat-host \
  --hulk0-dir /tmp/a2-roce-bandwidth-build.foZ9JJ \
  --hulk1-dir /tmp/a2-roce-bandwidth-build.Yelr5h --out results/new-run
```

Use a new name for each run. To reproduce the initial settings, append
`--bytes 1048576 --batch 16 --iterations 10000 --window 16 --cpus ''`.
If the temporary builds have been removed, rebuild on each node and pass
the new directories. Manual two-terminal commands are in
[BANDWIDTH.md](../../BANDWIDTH.md#build-and-run-on-the-two-hosts).

Each `.json` contains the exact commands, parameters, process exit codes,
metrics, and UTC completion timestamp. Matching `.source.log` and
`.target.log` files preserve output. `baseline-*` are initial measurements;
`final-{hbm,host}-{1,2,3}` are the sustained repeat sets; `reverse-*` check
the opposite direction. `controller-hbm` validates the saved SSH controller
and is separate from the three-run medians.

Local validation also passed: ten TCP protocol tests and benchmark-loop
tests for timing, byte accounting, completion, failures, and overflow.
