# A2 one-sided GET bandwidth benchmarks

`hbm_to_hbm_bw` and `host_to_hbm_bw` measure one-way payload bandwidth using
the existing demos' HCCL registration and `HcclBatchGet` path. Both are GNU
C89 and built directly by the Makefile. `transfer.c` owns the shared CANN
operations and peer protocol; `bench.c` handles warmup, submission windows,
completion and timing. No HIXL dependency or intermediate HBM payload buffer
is introduced for the host-source benchmark.

## Build and run on the two hosts

On both hosts, in a checkout containing the new files:

```sh
source /usr/local/Ascend/ascend-toolkit/set_env.sh
make
export HCCL_OP_EXPANSION_MODE=HOST
export ASCEND_GLOBAL_LOG_LEVEL=3 ASCEND_SLOG_PRINT_TO_STDOUT=0
```

The following commands pull from hulk-0 into hulk-1. The physical NPU IDs are
both 0, while communicator ranks are 0 and 1. These addresses were verified
for this environment; check them again if the network configuration changes.
Use the same benchmark executable and benchmark parameters on both peers.
Start the source first. Run one pair at a time on these NPUs.

```sh
# hulk@116.204.37.141: source host DDR, registered through NPU 0
taskset -c 144-167 ./build/host_to_hbm_bw --role source --device 0 \
  --host-ip 192.168.0.217 --npu-ip 29.184.165.65 \
  --peer-host-ip 192.168.0.63 --port 18100 \
  --bytes 16777216 --batch 16 --warmup 20 --iterations 1250 --window 16

# hulk@116.204.96.45: target NPU 0 HBM
taskset -c 144-167 ./build/host_to_hbm_bw --role target --device 0 \
  --host-ip 192.168.0.63 --npu-ip 29.184.110.95 \
  --peer-host-ip 192.168.0.217 --port 18100 \
  --bytes 16777216 --batch 16 --warmup 20 --iterations 1250 --window 16
```

To measure HBM-to-HBM, run the same pair with `hbm_to_hbm_bw` on both hosts
and `--port 18101`. To reverse the payload direction, exchange the roles;
keep each host's local addresses unchanged. For the host-source test, this
also moves the host DDR allocation to the new source host.

These are the measured throughput settings for this pair: a 256 MiB registered
region, 312.5 GiB timed payload, and roughly 14 seconds of measured transfer.
CPUs 144-167 are local to NPU 0 (NUMA node 6) on these hosts; this affinity is
machine-specific and is not an explicit memory-binding policy. The C programs
retain their smaller allocation defaults. See the [measurement report](results/20260922/README.md)
for initial bandwidth, parameter sweeps, repeated results, and limitations.

Alternatively, launch both peers from this checkout and capture their exit
codes, exact commands, raw logs, and metrics with the Python SSH controller:

```sh
python3 tools/run_hulk_bandwidth.py --kind host --name host-run-1 \
  --hulk0-dir /path/to/demo/on/hulk0 --hulk1-dir /path/to/demo/on/hulk1 \
  --out results/my-run
```

Use `--kind hbm` for HBM-to-HBM, or `--reverse` for hulk-1 to hulk-0. The
controller defaults to the throughput settings above; `--cpus ''` disables
affinity. It requires noninteractive SSH access to the documented hulk
addresses, bounds each remote process with a 240-second timeout, and refuses
to overwrite result files. The data path and bandwidth timer remain GNU C89.

## Parameters and accounting

| Option | Default | Meaning |
| --- | --- | --- |
| `--bytes` | 1048576 | Bytes per descriptor, not per entire benchmark |
| `--batch` | 16 | Nonoverlapping descriptors in each `HcclBatchGet`, 1..256 |
| `--warmup` | 10 | Untimed GET calls, 0 allowed |
| `--iterations` | 1000 | Timed GET calls, at least 1 |
| `--window` | 16 | GET calls submitted between stream waits, 1..1024 |
| `--timeout-ms` | 120000 | Per stream-wait timeout and HCCL timeout configuration, 120000..7200000 |

Each peer allocates/registers `bytes * batch` payload bytes once. Each GET
covers this whole allocation; subsequent GETs reuse it on the same stream.
The source stays immutable. The target also owns a host verification buffer
of the allocation size. The HBM source has a host initialization buffer.
The number of GET calls in a window does not multiply allocation size.
The peers compare their benchmark settings before preparing the transport.
Size and total-transfer arithmetic are checked for overflow.

The target:

1. Runs and drains warmup GETs using the chosen window size.
2. Reinitializes destination HBM with the complement of the source pattern,
   so successful warmup alone cannot make the final verification pass.
3. Starts a nanosecond-resolution `CLOCK_MONOTONIC` timer.
4. Submits measured GETs, synchronizing every window and after the final
   partial window. HCCL may apply backpressure during submission.
5. Stops the timer only after the final stream synchronization succeeds.
6. Copies HBM to host and checks every byte before printing bandwidth.
7. Completes the existing PASS/DONE and cleanup/CLOSED protocol.

```text
payload_bytes = bytes * batch * iterations
GB/s  = payload_bytes / elapsed_seconds / 1,000,000,000
GiB/s = payload_bytes / elapsed_seconds / 1,073,741,824
MiB/s = payload_bytes / elapsed_seconds / 1,048,576
Gb/s  = payload_bytes * 8 / elapsed_seconds / 1,000,000,000
```

Output starts with the actual configuration and then a `BANDWIDTH` line with
`source`, `payload_bytes`, `elapsed_s`, `GB/s`, `GiB/s`, `MiB/s`, `Gb/s`,
`amortized_us_per_get` and `verified_region_bytes`. Amortized time is elapsed
time divided by GET calls, not an individual-operation latency percentile.
A run shorter than one second suggests increasing `--iterations`.
Require a zero process exit and the final PASS on both peers; a bandwidth
line followed by cleanup/peer failure is not a fully successful run.

Timing includes host HCCL submission, backpressure, transport, per-call HCCL
fences and stream synchronization. It excludes allocation, registration,
connection setup, TCP handshakes, warmup, reinitialization, verification and
cleanup. Each transferred payload byte is counted once, not once at each
endpoint. Repeated calls count repeated transfers of the same registered
working set. Verification checks the final contents of all descriptors;
it does not validate a distinct pattern for every iteration.

`--batch` amortizes HCCL's final fenced notification across multiple READ
descriptors. `--window` amortizes the application's stream synchronization
across multiple GET calls. HCCL still inserts its own fence/notification wait
for **every GET call**. Neither option directly configures QP depth, creates
extra QPs/streams, or guarantees that window*batch READs execute concurrently.

The control connection retains the existing total deadline of six times
`--timeout-ms`, including initialization and verification. Keep the complete
run inside that deadline or increase the timeout. On uncertain transfer
failure, the existing lifetime guard avoids manually freeing live RDMA memory.

## Comparing with hccn_tool

Read-only inspection on 2026-09-22 found NPU 0 ports on both hosts reporting
`Speed: 200000 Mb/s`. Their raw one-way rate is 25 GB/s. The existing
`/tmp/recv_ib_read_bw.txt` on hulk-0 reports:

```text
RDMA_Read BW Test
Number of qps: 1      Connection type: RC
CQ Moderation: 1     Mtu: 4096[B]
Outstand reads: 128
#bytes       #iterations       BW average[MB/sec]      MsgRate[Mpps]
1048576      93205             23302.26                0.023302
```

Despite the `MB/sec` heading, the installed tool's help specifies **MiB/sec**.
The message-rate column also agrees: 23,302 one-MiB messages per second is
about 24.43 decimal GB/s, or 195.47 Gb/s (97.7% of the raw 200-Gb/s rate).
The saved write result is 23293.09 MiB/s; the saved SEND report is a later
bidirectional run, 46493.43 MiB/s aggregate. These are existing reports,
not measurements made with the new benchmarks.

The user's one-way SEND command was:

```sh
# Receiver on hulk-0
hccn_tool -i 0 -roce_test ib_send_bw -d hns_0 -s 1048576 -D 8 -tcp
# Sender on hulk-1
hccn_tool -i 0 -roce_test ib_send_bw -d hns_0 -s 1048576 -D 8 -tcp address 29.184.165.65
```

This is a sustained verbs-level SEND test. The local driver wrapper sends a
`DS_START_PERFTEST` request to the selected NPU's service; it is not an HCCL
BatchGet loop. Large messages, outstanding requests and an MTU of 4096 allow
the hardware to keep the link busy while amortizing submission/protocol costs.
`-tcp` selects connection setup; the payload remains RDMA over the embedded
RoCE NIC. The available reports do not establish the perftest buffer's
physical memory placement, so do not treat them as a host-DDR-to-HBM result.

Use `ib_read_bw` as the closer verbs-level baseline for these GET benchmarks.
The HCCL path adds address lookup, per-call task submission and a fenced
notification/stream wait. The host-source path also depends on host-memory
placement and the host-to-NPU PCIe path. Neither benchmark is promised to
match the NIC-only baseline. Compare the same payload direction; the command
above sends B to A, while the benchmark examples transfer A to B.

Try matched runs with `--batch 1 --window 1`, then `--batch 16 --window 1`,
then `--batch 16 --window 16`. Sweep descriptor sizes such as 64 KiB, 1 MiB
and 16 MiB. Adjust iterations to obtain several seconds of measured time,
and repeat runs to assess variability. Use the same logging level on both
hosts; verbose logging can affect submission throughput. The benchmark
preserves the supplied HCCL expansion-mode setting and does not tune NICs,
NUMA placement, queues or other running workloads automatically.

Local source references:

- [hccn_tool options and bandwidth-unit definition](../driver/src/custom/network/hccn/ascend910B/hccn_product_ext/cmd_ext.c)
- [Device-service perftest request](../driver/src/custom/network/hccn/ascend910B/hccn_product_ext/ds_net_ext.c)
- [HCCL READ posting and final fence](../hcomm/src/legacy/ascend910/platform/resource/transport/onesided/transport_roce_mem.cc)

## Validation

`make test` needs no CANN installation. It runs the existing TCP control
tests and tests the actual benchmark loop with deterministic callbacks:
warmup/reset exclusion, exact byte counts, full and partial submission
windows, final completion, failure handling and overflow rejection. These
callbacks do not emulate RDMA or prove hardware bandwidth. Local GNU C89
compilation against the checkout's ACL headers checks the CANN-facing code;
real transfer measurements must be made on the A2 pair.

On 2026-09-22 all four executables compiled and linked against installed
CANN 8.5.2 on both hulk nodes. Both bandwidth programs were then run against
NPU 0 on each node, including tuning sweeps and repeated measurements with
full final-buffer verification and successful cleanup on both peers. Raw
logs and exact commands are in [results/20260922](results/20260922/README.md).
The ten CPU protocol tests and benchmark-loop tests also passed.
