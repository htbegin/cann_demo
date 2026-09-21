# Ascend A2 embedded-RoCE demos (GNU C89)

Two standalone programs share `transfer.c` and `control.c`:

| Program | Source (rank 0) | Target (rank 1) |
| --- | --- | --- |
| `hbm_to_hbm` | NPU 1 HBM | NPU X HBM, on the same or a second host |
| `host_to_hbm` | Host A DDR, registered through NPU 1 | NPU X HBM on host B |

Both use **receiver-initiated RDMA GET**. “Source” describes where the data
lives; the target issues the transfer. The second demo interprets the requested
destination NPU X as its HBM, not host B's DDR.

```text
hbm_to_hbm:
  source HBM ---- source NPU embedded RoCE === target NPU embedded RoCE ---- target HBM

host_to_hbm:
  source host DDR -- mapped/registered through source NPU's device address space
                  -- source NPU embedded RoCE === target NPU embedded RoCE -- target HBM

  host TCP control: endpoint metadata, source address, readiness and completion only
```

The host-source process allocates the payload with `aclrtMallocHost`; it does
not allocate an HBM payload or copy the payload into source HBM. HCOMM handles
mapping host memory into the device address space and registering it with the
NPU-side NIC. The embedded NIC accesses host DDR across the device/host fabric.
This requires the driver to support that mapping; it is not ordinary host-NIC
`ibv_reg_mr`, and does not use an external NIC's peer-memory path.

All demo implementation files are C, compiled with `-std=gnu89`. There is no
HIXL dependency, Python data path, MPI requirement, or C++ wrapper.

## Prerequisites

- Two Ascend A2 NPUs (Ascend910B), with their embedded RoCE ports configured and
  mutually reachable. Even same-host operation needs the RoCE network path.
- A matching installed CANN/driver/firmware stack providing
  `HcclCommInitClusterInfoMemConfig`, `HcclRegisterGlobalMem`,
  `HcclCommBindMem`, `HcclCommPrepare`, `HcclBatchGet`, and
  `aclrtSynchronizeStreamWithTimeout`. The reference LMCache backend targets
  CANN 8.5+. This implementation has **not been linked or run on an A2 system**.
- Host control-network connectivity between the processes and an available TCP
  port (default 18000). HCCL also establishes its own device-side connections;
  permitting only TCP 18000 is insufficient for the RoCE data path.
- Driver-managed host-memory registration support. LMCache recommends HDK
  25.5.0+; older drivers have a smaller host-registration limit. The default
  payload here is only 1 MiB.

Query the physical NPU's RoCE IP using your driver tooling, for example:

```sh
/usr/local/Ascend/driver/tools/hccn_tool -i 1 -ip -g
/usr/local/Ascend/driver/tools/hccn_tool -i 1 -link -g
```

`--device` is the **logical** ACL device ID, affected by device visibility
settings. The program queries its physical ID for the rank table. `hccn_tool`
uses the physical ID. Ensure `--npu-ip` belongs to that physical NPU.
Use the same `--host-ip` for both processes on one host: it is also the rank
table's server identity. Use real reachable host IPs, not loopback addresses,
for hardware runs. Device IDs can coincide on different hosts.

The program sets `HCCL_INTRA_ROCE_ENABLE=1` and `HCCL_INTRA_PCIE_ENABLE=0` before
ACL/HCCL initialization. The inspected A2 one-sided implementation selects
RDMA when the former is set, including on the same server. This expresses the
requested transport; confirm the actual route in HCCL logs/NPU port counters
on the installed stack. A payload PASS alone is not independent proof of the
physical route. Product and CANN-version support still apply.

## Build

Source the environment script for the CANN installation on **each** host:

```sh
source /usr/local/Ascend/ascend-toolkit/set_env.sh
cd /home/begin/code/cann/demo
make -j CANN_ROOT=/usr/local/Ascend/ascend-toolkit/latest
```

Adjust the installation and source paths to your machine. The Makefile links
`libhcomm`, `libhccl`, and `libascendcl`, using the C compiler/linker selected by
`CC` (for example, `make CC=gcc`). `CANN_ROOT` defaults to `ASCEND_HOME_PATH`
when set, otherwise `/usr/local/Ascend/ascend-toolkit/latest`. For an installation
using `libacl_rt` instead of `libascendcl`, pass `ACL_LIB=acl_rt`. `CPPFLAGS`,
`CFLAGS`, `LDFLAGS`, and `LDLIBS` are supported; the GNU C89 and warning flags
are always applied. `SDK_CPPFLAGS`, `SDK_LDFLAGS`, and `SDK_LDLIBS` can override
the SDK search paths and libraries. `make clean` removes generated objects and
executables. Source the environment script before execution too, so the
vendor libraries and their dependencies can be found. An undefined one-sided
API symbol indicates a mismatched/unsupported installed stack; these programs
do not substitute a staged or TCP payload transfer.

The vendor's one-sided headers have C-linkage functions but contain C++-only
constructs (`const` array bounds, `nullptr`, and C++ includes). `hccl_c_abi.h`
provides the small C-compatible ABI subset used here. It uses a size-tagged
version-2 `HcclCommConfig` prefix, which includes the communicator name, instead
of guessing the size of newer configurations. The inspected `CommConfig::Load`
and `SetConfigByVersion` support this prefix. Compile-time size/offset checks
require a 64-bit target. The layouts and enum values were also compared against
the local HCOMM headers in a separate compile-only check. This is compatibility
code tied to that ABI, not a claim that every future CANN release preserves it.

## Demo 1: HBM to HBM on one host

Example addresses below are placeholders. Assume host A is `10.10.0.10`,
NPU 1's embedded NIC is `192.168.100.11`, and NPU 3's is `192.168.100.13`.
Start source and target in separate terminals, using the same binary and size.

```sh
# Host A, terminal 1: source NPU 1
./build/hbm_to_hbm --role source --device 1 \
  --host-ip 10.10.0.10 --npu-ip 192.168.100.11 \
  --peer-host-ip 10.10.0.10 --port 18000 --bytes 1048576

# Host A, terminal 2: target NPU 3
./build/hbm_to_hbm --role target --device 3 \
  --host-ip 10.10.0.10 --npu-ip 192.168.100.13 \
  --peer-host-ip 10.10.0.10 --port 18000 --bytes 1048576
```

## Demo 1: HBM to HBM across two hosts

Assume host B is `10.10.0.20` and its NPU 3's NIC is `192.168.100.23`.

```sh
# Host A: source NPU 1
./build/hbm_to_hbm --role source --device 1 \
  --host-ip 10.10.0.10 --npu-ip 192.168.100.11 \
  --peer-host-ip 10.10.0.20 --port 18000 --bytes 1048576

# Host B: target NPU 3
./build/hbm_to_hbm --role target --device 3 \
  --host-ip 10.10.0.20 --npu-ip 192.168.100.23 \
  --peer-host-ip 10.10.0.10 --port 18000 --bytes 1048576
```

## Demo 2: host A DDR through NPU 1 to host B NPU X HBM

```sh
# Host A: host buffer registered through NPU 1
./build/host_to_hbm --role source --device 1 \
  --host-ip 10.10.0.10 --npu-ip 192.168.100.11 \
  --peer-host-ip 10.10.0.20 --port 18000 --bytes 1048576

# Host B: target NPU 3
./build/host_to_hbm --role target --device 3 \
  --host-ip 10.10.0.20 --npu-ip 192.168.100.23 \
  --peer-host-ip 10.10.0.10 --port 18000 --bytes 1048576
```

Replace 3 with X and supply X's NIC IP. Run one pair at a time per selected NPU.
`--timeout-ms` defaults to 30000; it controls stream completion and is rounded
up to seconds for HCCL prepare/connect/execute settings. The control connection
has a total budget of six times that value, including initialization time.
These settings do not promise a hard wall-clock bound on every vendor cleanup
call. All control sockets use bounded nonblocking I/O and detect premature EOF.

Success on both processes ends with:

```text
PASS: all 1048576 bytes verified in target HBM; both peers released registrations
```

The target starts with the complement of the expected pattern, reads the source
region, copies its HBM to a CPU verification buffer, and checks every byte. The
initial HBM fill and final verification copy are outside the RDMA transfer.
The printed transfer time includes submission and stream completion; this is a
correctness demo, not a bandwidth benchmark.

## Shared API sequence and ownership

1. Set the local NPU context and allocate the region. Source HBM and target HBM
   use `aclrtMalloc`; source host DDR uses `aclrtMallocHost`.
2. `HcclRegisterGlobalMem` records the region and its actual memory type.
3. Exchange versioned metadata over host TCP and generate an identical A2
   rank table on each side with explicit physical device IDs and RoCE IPs.
4. `HcclCommInitClusterInfoMemConfig` creates the one-sided communicator.
5. `HcclCommBindMem`, then `HcclCommPrepare` on **both** processes, register and
   exchange the NIC memory-access information. A TCP address alone does not
   authorize an RDMA access.
6. After both peers report READY, the target calls `HcclBatchGet` with local
   target HBM and the original remote source VA, then
   `aclrtSynchronizeStreamWithTimeout`. HCOMM translates registered host VAs
   to their device-visible addresses internally.
7. The target verifies the data and reports completion. Both then unbind memory,
   destroy the communicator, deregister memory, destroy the stream, free the
   buffers, reset the device, and finalize ACL. A final acknowledgement confirms
   peer cleanup before reporting PASS.

Do not mutate/free the source region or deregister either region while the GET
is outstanding. On a transfer/control failure with uncertain outstanding DMA,
the demo exits without manually freeing registered allocations, leaving resource
reclamation to driver-managed process teardown. Cleanup errors return failure
and stop further releases that could free still-registered memory. The TCP
protocol is a trusted-cluster demo protocol with no authentication.

To use an existing application region, replace allocation/pattern initialization
in `initialize`, preserve the same registration/type and lifetime rules, and
wait for its producer stream before publishing readiness. Do not CPU-dereference
an HBM pointer. This demo deliberately transfers the full registered allocation;
subregion transfers must keep address and length within its registered bounds.

## Why no HIXL staging pool?

The final GNU C89 implementation calls HCCL/HCOMM directly and has no HIXL pool.
In a HIXL variant, `BufferPool="0:0"` would disable the optional staging pool so
the experiment depends on explicitly registered source and target memory.
It is **not required for RoCE**. HIXL can choose its direct path for registered
regions even when the pool is enabled; the pool supports cases including
unregistered host buffers and registration-capacity constraints. Enabling it is
a useful separate staging experiment, not a necessary part of these demos.

## Validation without hardware

```sh
make test
```

This builds the real control code and a GNU C89 probe. Python standard-library
tests cover metadata rejection, same-host/two-host rank-table generation,
fragmented TCP frames, readiness/completion exchange, early EOF, malformed
frames, and timeouts. They use loopback TCP only and do not emulate RDMA.

Validation performed in the source-only workspace: GNU C89 syntax checks of
all demo C files against the local ACL headers, warnings-as-errors compilation
of the CPU control code, ten passing CPU protocol tests, and an ABI layout check
against local HCOMM headers. Full vendor-library linking and all three A2
hardware scenarios remain untested because this workspace has no installed
CANN runtime or A2 device.

## References

- [LMCache Ascend P2P reference](https://github.com/LMCache/LMCache-Ascend/blob/main/examples/kv_cache_reuse/share_across_instances/p2p_sharing/README.md):
  remote host-to-device pulls over RoCE and host-registration constraints.
- Local reference used for the C API sequence:
  `/home/begin/code/vllm/lmcache-ascend/csrc/hcomm_onesided/bindings.cpp` and
  `lmcache_ascend/v1/transfer_channel/hcomm_onesided_runtime.py` in that checkout.
- [One-sided API declarations](../hcomm/pkg_inc/legacy/hccl/hccl_one_sided_services.h).
- [A2 RDMA selection and full-mesh preparation](../hcomm/src/legacy/ascend910/framework/communicator/impl/one_sided_service/hccl_one_sided_service.cc).
- [Device-side host-MR mapping](../hcomm/src/legacy/ascend910/platform/resource/rma_buffer/local_rdma_rma_buffer_impl.cc)
  and [registered address translation](../hcomm/src/legacy/ascend910/platform/resource/transport/onesided/transport_roce_mem.cc).
- [HIXL registration and staging-pool documentation](../hixl/docs/zh/api/cpp/HIXL-interface.md).
