#define _POSIX_C_SOURCE 200809L
#include "demo.h"
#include "control.h"
#include "hccl_c_abi.h"
#include "acl/acl.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct options {
    int source, device, timeout_ms;
    uint16_t port;
    size_t bytes;
    const char *host_ip, *npu_ip, *peer_host_ip;
};
struct session {
    A2Comm comm;
    aclrtStream stream;
    void *buffer, *registration;
    unsigned char *scratch;
    int acl_initialized, device_set, host, bound, exposed, quiesced;
};

/* All functions using CHECK/REQUIRE have an out label and default failure. */
#define CHECK(call) do { \
    int32_t code_ = (int32_t)(call); \
    if (code_) { fprintf(stderr, "%s failed: %" PRId32 "\n", #call, code_); goto out; } \
} while (0)
#define REQUIRE(call) do { \
    if ((call) != 0) { fprintf(stderr, "%s: %s\n", #call, strerror(errno)); goto out; } \
} while (0)

static void usage(const char *program)
{
    printf("Usage: %s --role source|target --device ID --host-ip IP --npu-ip IP\n"
           "  --peer-host-ip IP [--port 18000] [--bytes 1048576] [--timeout-ms 30000]\n"
           "Start source first. Target issues an RDMA GET into target HBM.\n"
           "host-ip is the host control IP; npu-ip is this NPU's embedded RoCE IP.\n", program);
}

static int parse_options(int argc, char **argv, struct options *o)
{
    static const char *keys[] = {"--role", "--device", "--host-ip", "--npu-ip", "--peer-host-ip",
                                 "--port", "--bytes", "--timeout-ms"};
    const char *values[8] = {NULL, NULL, NULL, NULL, NULL, "18000", "1048576", "30000"};
    unsigned int seen = 0;
    uint64_t number;
    int i, k;
    for (i = 1; i < argc; i += 2) {
        for (k = 0; k < 8 && strcmp(argv[i], keys[k]); ++k) {}
        if (k == 8 || i + 1 >= argc || (seen & (1U << k))) {
            fprintf(stderr, "Unknown, duplicate, or incomplete option: %s\n", argv[i]);
            return -1;
        }
        values[k] = argv[i + 1];
        seen |= 1U << k;
    }
    if ((seen & 31U) != 31U || (strcmp(values[0], "source") && strcmp(values[0], "target"))) {
        fprintf(stderr, "Specify all five required options and role source or target\n");
        return -1;
    }
    o->source = !strcmp(values[0], "source");
    if (parse_number(values[1], 0, INT32_MAX, &number)) return -1;
    o->device = (int)number;
    o->host_ip = values[2]; o->npu_ip = values[3]; o->peer_host_ip = values[4];
    if (!valid_ip(o->host_ip) || !valid_ip(o->npu_ip) || !valid_ip(o->peer_host_ip)) return -1;
    if (parse_number(values[5], 1, UINT16_MAX, &number)) return -1;
    o->port = (uint16_t)number;
    if (parse_number(values[6], 1, SIZE_MAX, &number)) return -1;
    o->bytes = (size_t)number;
    if (parse_number(values[7], 1, INT_MAX / 6, &number)) return -1;
    o->timeout_ms = (int)number;
    return 0;
}

static unsigned char pattern(size_t offset)
{
    uint64_t mixed = ((uint64_t)offset + 1) * UINT64_C(0x9e3779b97f4a7c15);
    return (unsigned char)(mixed ^ (mixed >> 17) ^ (mixed >> 41));
}

static int initialize(struct session *s, const struct options *o, enum source_memory kind,
                      struct metadata *local)
{
    const char *soc;
    char timeout_seconds[24];
    int32_t physical = -1;
    A2Mem memory;
    size_t i;
    snprintf(timeout_seconds, sizeof(timeout_seconds), "%d", (o->timeout_ms + 999) / 1000);
    /* IsUsedRdma() selects RDMA for A2 when INTRA_ROCE is set, even on one host. */
    REQUIRE(setenv("HCCL_INTRA_ROCE_ENABLE", "1", 1));
    REQUIRE(setenv("HCCL_INTRA_PCIE_ENABLE", "0", 1));
    REQUIRE(setenv("HCCL_CONNECT_TIMEOUT", timeout_seconds, 1));
    REQUIRE(setenv("HCCL_EXEC_TIMEOUT", timeout_seconds, 1));
    CHECK(aclInit(NULL));
    s->acl_initialized = 1;
    CHECK(aclrtSetDevice(o->device));
    s->device_set = 1;
    soc = aclrtGetSocName();
    if (!soc || strncmp(soc, "Ascend910B", 10)) {
        fprintf(stderr, "This demo targets A2 (Ascend910B), got %s\n", soc ? soc : "unknown SoC");
        goto out;
    }
    CHECK(aclrtGetPhyDevIdByLogicDevId(o->device, &physical));
    if (physical < 0) { fprintf(stderr, "Invalid physical device ID\n"); goto out; }
    s->host = o->source && kind == SOURCE_HOST;
    if (s->host) {
        CHECK(aclrtMallocHost(&s->buffer, o->bytes));
        /* No source HBM allocation or H2D staging in the host-memory demo. */
        for (i = 0; i < o->bytes; ++i) ((unsigned char *)s->buffer)[i] = pattern(i);
    } else {
        CHECK(aclrtMalloc(&s->buffer, o->bytes, ACL_MEM_MALLOC_HUGE_FIRST));
        s->scratch = malloc(o->bytes);
        if (!s->scratch) { perror("malloc verification buffer"); goto out; }
        for (i = 0; i < o->bytes; ++i) s->scratch[i] = o->source ? pattern(i) : (unsigned char)~pattern(i);
        CHECK(aclrtMemcpy(s->buffer, o->bytes, s->scratch, o->bytes, ACL_MEMCPY_HOST_TO_DEVICE));
    }
    CHECK(aclrtCreateStream(&s->stream));
    memset(&memory, 0, sizeof(memory));
    memory.type = s->host ? 1 : 0;
    memory.addr = s->buffer;
    memory.size = o->bytes;
    CHECK(HcclRegisterGlobalMem(&memory, &s->registration));
    memset(local, 0, sizeof(*local));
    local->kind = kind; local->bytes = o->bytes; local->address = (uintptr_t)s->buffer;
    local->physical_device = (uint32_t)physical; local->rank = o->source ? 0 : 1;
    snprintf(local->host_ip, sizeof(local->host_ip), "%s", o->host_ip);
    snprintf(local->npu_ip, sizeof(local->npu_ip), "%s", o->npu_ip);
    printf("%s: logical=%d physical=%" PRId32 " host=%s NPU_RoCE=%s memory=%s bytes=%zu\n",
           o->source ? "source" : "target", o->device, physical, o->host_ip, o->npu_ip,
           s->host ? "HOST" : "HBM", o->bytes);
    fflush(stdout);
    return 0;
out:
    return -1;
}

static int prepare(struct session *s, const struct options *o, const struct metadata *local,
                   const struct metadata *peer)
{
    char rank_table[1024];
    A2CommConfigV2 config;
    A2PrepareConfig prep;
    REQUIRE(make_rank_table(o->source ? local : peer, o->source ? peer : local,
                            rank_table, sizeof(rank_table)));
    printf("Rank table: %s\n", rank_table);
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.magic = UINT32_C(0xf0f0f0f0);
    config.version = 2;
    config.buffer_size = UINT32_MAX;
    config.deterministic = UINT32_MAX;
    snprintf(config.name, sizeof(config.name), "a2_roce_demo_%u", (unsigned)o->port);
    CHECK(HcclCommInitClusterInfoMemConfig(rank_table, (uint32_t)local->rank, &config, &s->comm));
    CHECK(HcclCommBindMem(s->comm, s->registration));
    s->bound = 1;
    memset(&prep, 0, sizeof(prep)); /* FULLMESH */
    /* Both processes call Prepare: registers NIC MRs and exchanges access info. */
    CHECK(HcclCommPrepare(s->comm, &prep, (o->timeout_ms + 999) / 1000));
    return 0;
out:
    return -1;
}

static int pull_and_verify(struct session *s, const struct options *o, const struct metadata *peer)
{
    A2OneSideOp op;
    int64_t start;
    size_t i;
    memset(&op, 0, sizeof(op));
    op.local_addr = s->buffer;
    /* This is the original source VA. HCOMM resolves its registered device VA. */
    op.remote_addr = (void *)(uintptr_t)peer->address;
    op.count = o->bytes;
    op.data_type = 7; /* HCCL_DATA_TYPE_UINT8 */
    start = monotonic_ms();
    CHECK(HcclBatchGet(s->comm, 0, &op, 1, s->stream));
    CHECK(aclrtSynchronizeStreamWithTimeout(s->stream, o->timeout_ms));
    s->quiesced = 1;
    printf("GET complete: %zu bytes, %" PRId64 " ms (setup and verification excluded)\n",
           o->bytes, monotonic_ms() - start);
    CHECK(aclrtMemcpy(s->scratch, o->bytes, s->buffer, o->bytes, ACL_MEMCPY_DEVICE_TO_HOST));
    for (i = 0; i < o->bytes; ++i) {
        if (s->scratch[i] != pattern(i)) {
            fprintf(stderr, "Mismatch at byte %zu: expected %u, got %u\n",
                    i, (unsigned)pattern(i), (unsigned)s->scratch[i]);
            return 1;
        }
    }
    return 0;
out:
    return -1;
}

static int cleanup(struct session *s, const struct options *o)
{
    /* Stop on a release error. Do not free an allocation whose MR may survive. */
    if (s->bound) { CHECK(HcclCommUnbindMem(s->comm, s->registration)); s->bound = 0; }
    if (s->comm) { CHECK(HcclCommDestroy(s->comm)); s->comm = NULL; }
    if (s->registration) { CHECK(HcclDeregisterGlobalMem(s->registration)); s->registration = NULL; }
    if (s->stream) { CHECK(aclrtDestroyStream(s->stream)); s->stream = NULL; }
    if (s->buffer) {
        CHECK(s->host ? aclrtFreeHost(s->buffer) : aclrtFree(s->buffer));
        s->buffer = NULL;
    }
    free(s->scratch); s->scratch = NULL;
    if (s->device_set) { CHECK(aclrtResetDevice(o->device)); s->device_set = 0; }
    if (s->acl_initialized) { CHECK(aclFinalize()); s->acl_initialized = 0; }
    return 0;
out:
    return -1;
}

int run_demo(int argc, char **argv, enum source_memory memory)
{
    struct options options;
    struct session session;
    struct control control;
    struct metadata local, peer;
    char message[CONTROL_FRAME_SIZE];
    int result = 1, verified = 0, transfer_result, cleanup_attempted = 0;
    memset(&session, 0, sizeof(session));
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(argv[0]); return 0; }
    if (parse_options(argc, argv, &options)) { usage(argv[0]); return 1; }
    control_init(&control, options.timeout_ms * 6);
    if (initialize(&session, &options, memory, &local)) goto out;
    if (options.source) REQUIRE(control_accept(&control, options.host_ip, options.port));
    else REQUIRE(control_connect(&control, options.peer_host_ip, options.port));
    REQUIRE(metadata_encode(&local, message));
    REQUIRE(control_send(&control, message));
    REQUIRE(control_receive(&control, message));
    REQUIRE(metadata_decode(message, &peer));
    REQUIRE(metadata_validate(&local, &peer, options.peer_host_ip));
    if (prepare(&session, &options, &local, &peer)) goto out;
    /* No payload operation before both sides have prepared their registrations. */
    session.exposed = 1;
    REQUIRE(control_send(&control, "READY"));
    REQUIRE(control_receive(&control, message));
    if (strcmp(message, "READY")) { fprintf(stderr, "Expected READY\n"); goto out; }
    if (options.source) {
        REQUIRE(control_receive(&control, message));
        if (strcmp(message, "PASS") && strcmp(message, "FAIL")) {
            fprintf(stderr, "Expected verified completion\n"); goto out;
        }
        session.quiesced = 1;
        verified = !strcmp(message, "PASS");
        REQUIRE(control_send(&control, "DONE"));
    } else {
        transfer_result = pull_and_verify(&session, &options, &peer);
        if (transfer_result < 0) goto out;
        verified = transfer_result == 0;
        REQUIRE(control_send(&control, verified ? "PASS" : "FAIL"));
        REQUIRE(control_receive(&control, message));
        if (strcmp(message, "DONE")) { fprintf(stderr, "Expected DONE\n"); goto out; }
    }
    cleanup_attempted = 1;
    if (cleanup(&session, &options)) goto out;
    REQUIRE(control_send(&control, "CLOSED"));
    REQUIRE(control_receive(&control, message));
    if (strcmp(message, "CLOSED")) { fprintf(stderr, "Expected CLOSED\n"); goto out; }
    if (verified) {
        printf("PASS: all %zu bytes verified in target HBM; both peers released registrations\n", options.bytes);
        result = 0;
    }
out:
    if (session.exposed && !session.quiesced) {
        /* A broken control socket/timeout does not prove remote DMA has stopped.
         * Leave registered memory intact for driver-managed process teardown. */
        fprintf(stderr, "Transfer state uncertain; exiting without manually releasing live registered memory\n");
        fflush(NULL);
        _exit(1);
    }
    if (!cleanup_attempted && cleanup(&session, &options)) result = 1;
    control_close(&control);
    return result;
}
