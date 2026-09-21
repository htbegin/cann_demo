#ifndef A2_HCCL_C_ABI_H
#define A2_HCCL_C_ABI_H

/* GNU C89 view of the vendor's C-linkage one-sided API. Its original headers
 * contain C++ array bounds and nullptr, so cannot be included directly in C.
 * Definitions: hcomm/include/hccl/hccl_types.h, include/hcomm_res_defs.h,
 * pkg_inc/legacy/hccl/hccl_one_sided_services.h, and op_base/src/op_base.h.
 *
 * The config is the version-2 prefix through hcclCommName. CommConfig::Load
 * copies only size bytes; SetConfigByVersion reads fields for that version.
 * This targets the 64-bit CANN 8.5+ A2 one-sided ABI, not arbitrary SDKs.
 * Only C ABI structs are mirrored, never C++ classes. See README for checks.
 */
#include <stddef.h>
#include <stdint.h>

typedef void *A2Comm;
typedef struct {
    size_t size;
    uint32_t magic;
    uint32_t version;
    uint64_t reserved;
    uint32_t buffer_size;
    uint32_t deterministic;
    char name[128];
} A2CommConfigV2;
typedef struct {
    int32_t type; /* DEVICE=0, HOST=1; called CommMem or HcclMem in the SDK */
    void *addr;
    uint64_t size;
} A2Mem;
typedef struct {
    int32_t topology; /* FULLMESH=0 */
    uint64_t reserved[3];
} A2PrepareConfig;
typedef struct {
    void *local_addr;
    void *remote_addr;
    uint64_t count;
    int32_t data_type; /* UINT8=7 */
} A2OneSideOp;

typedef char a2_pointer_abi[(sizeof(void *) == 8 && sizeof(size_t) == 8) ? 1 : -1];
typedef char a2_config_abi[(sizeof(A2CommConfigV2) == 160 && offsetof(A2CommConfigV2, name) == 32) ? 1 : -1];
typedef char a2_mem_abi[(sizeof(A2Mem) == 24 && offsetof(A2Mem, addr) == 8) ? 1 : -1];
typedef char a2_prepare_abi[(sizeof(A2PrepareConfig) == 32 && offsetof(A2PrepareConfig, reserved) == 8) ? 1 : -1];
typedef char a2_op_abi[(sizeof(A2OneSideOp) == 32 && offsetof(A2OneSideOp, data_type) == 24) ? 1 : -1];

#ifndef A2_ABI_TYPES_ONLY
extern int32_t HcclCommInitClusterInfoMemConfig(const char *, uint32_t, A2CommConfigV2 *, A2Comm *);
extern int32_t HcclCommDestroy(A2Comm);
extern int32_t HcclRegisterGlobalMem(const A2Mem *, void **);
extern int32_t HcclDeregisterGlobalMem(void *);
extern int32_t HcclCommBindMem(A2Comm, void *);
extern int32_t HcclCommUnbindMem(A2Comm, void *);
extern int32_t HcclCommPrepare(A2Comm, const A2PrepareConfig *, int);
extern int32_t HcclBatchGet(A2Comm, uint32_t, A2OneSideOp *, uint32_t, void *);
#endif
#endif
