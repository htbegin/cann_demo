/* Test scheduling/accounting only; callbacks do not emulate NPU or RDMA. */
#include "bench.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fake {
    uint32_t submissions, syncs, resets, clocks, pending, max_pending;
    uint32_t fail_submit, fail_sync;
    int fail_reset, fail_clock;
    int64_t time;
};

static int submit(void *context)
{
    struct fake *f = context;
    ++f->submissions;
    ++f->pending;
    if (f->pending > f->max_pending) f->max_pending = f->pending;
    f->time += 10;
    return f->submissions == f->fail_submit ? -1 : 0;
}

static int sync_calls(void *context)
{
    struct fake *f = context;
    ++f->syncs;
    f->time += 100;
    if (f->syncs == f->fail_sync) return -1;
    f->pending = 0;
    return 0;
}

static int reset(void *context)
{
    struct fake *f = context;
    assert(f->pending == 0);
    ++f->resets;
    f->time += 10000;
    return f->fail_reset ? -1 : 0;
}

static int64_t now_ns(void *context)
{
    struct fake *f = context;
    ++f->clocks;
    assert(f->pending == 0);
    return f->fail_clock ? -1 : f->time;
}

int main(void)
{
    const struct bench_ops ops = {submit, sync_calls, reset, now_ns};
    struct bench_config c = {1048576, 16, 3, 5, 2};
    struct bench_result r;
    struct fake f;
    size_t region;

    memset(&f, 0, sizeof(f));
    assert(bench_run(&c, &ops, &f, &r) == 0);
    assert(f.submissions == 8 && f.syncs == 5 && f.resets == 1);
    assert(f.clocks == 2 && f.max_pending == 2);
    assert(r.payload_bytes == UINT64_C(83886080));
    /* Only five measured submissions and three measured syncs; no warmup/reset. */
    assert(r.elapsed_ns == 350);

    memset(&f, 0, sizeof(f));
    c.warmup = 0; c.window = 16;
    assert(bench_run(&c, &ops, &f, &r) == 0);
    assert(f.submissions == 5 && f.syncs == 1 && f.resets == 1);
    assert(r.elapsed_ns == 150 && r.payload_bytes == UINT64_C(83886080));

    memset(&f, 0, sizeof(f));
    c.window = 1;
    assert(bench_run(&c, &ops, &f, &r) == 0);
    assert(f.syncs == 5 && f.max_pending == 1 && r.elapsed_ns == 550);

    memset(&f, 0, sizeof(f));
    f.fail_submit = 2;
    assert(bench_run(&c, &ops, &f, &r) == -1);
    assert(f.submissions == 2 && f.syncs == 1);
    assert(r.payload_bytes == 0 && r.elapsed_ns == 0);

    memset(&f, 0, sizeof(f));
    f.fail_sync = 1;
    assert(bench_run(&c, &ops, &f, &r) == -1 && r.payload_bytes == 0);
    assert(f.pending == 1 && f.clocks == 1);

    memset(&f, 0, sizeof(f));
    c.warmup = 3; f.fail_sync = 1;
    assert(bench_run(&c, &ops, &f, &r) == -1 && f.clocks == 0);
    assert(f.resets == 0 && r.payload_bytes == 0);

    memset(&f, 0, sizeof(f));
    c.warmup = 0; f.fail_reset = 1;
    assert(bench_run(&c, &ops, &f, &r) == -1 && f.submissions == 0);
    memset(&f, 0, sizeof(f));
    f.fail_clock = 1;
    assert(bench_run(&c, &ops, &f, &r) == -1 && f.submissions == 0);

    c.message_bytes = SIZE_MAX; c.batch = 2;
    assert(bench_validate(&c, &region) == -1 && errno == EOVERFLOW);
    c.message_bytes = (size_t)(UINT64_MAX / 2 + 1); c.batch = 1; c.iterations = 2;
    assert(bench_validate(&c, &region) == -1 && errno == EOVERFLOW);
    c.message_bytes = 1; c.batch = 257;
    assert(bench_validate(&c, &region) == -1 && errno == EINVAL);
    c.batch = 1; c.window = 0;
    assert(bench_validate(&c, &region) == -1);
    c.window = 1025;
    assert(bench_validate(&c, &region) == -1);
    c.window = 1; c.iterations = 0;
    assert(bench_validate(&c, &region) == -1);
    puts("PASS: bandwidth scheduling, timing exclusions, failures and overflow checks");
    return 0;
}
