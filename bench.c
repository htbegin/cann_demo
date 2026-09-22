#include "bench.h"
#include <errno.h>

int bench_validate(const struct bench_config *c, size_t *region_bytes)
{
    size_t bytes;
    if (!c->message_bytes || !c->batch || c->batch > 256 ||
        !c->iterations || !c->window || c->window > 1024) {
        errno = EINVAL;
        return -1;
    }
    if (c->message_bytes > SIZE_MAX / c->batch) {
        errno = EOVERFLOW;
        return -1;
    }
    bytes = c->message_bytes * c->batch;
    if ((uint64_t)bytes > UINT64_MAX / c->iterations) {
        errno = EOVERFLOW;
        return -1;
    }
    *region_bytes = bytes;
    return 0;
}

static int run_calls(uint32_t calls, uint32_t window,
                     const struct bench_ops *ops, void *context)
{
    uint32_t count, i;
    while (calls) {
        count = calls < window ? calls : window;
        for (i = 0; i < count; ++i) {
            if (ops->submit(context)) return -1;
        }
        /* Also drain a final, partially filled window before counting bytes. */
        if (ops->sync(context)) return -1;
        calls -= count;
    }
    return 0;
}

int bench_run(const struct bench_config *c, const struct bench_ops *ops,
              void *context, struct bench_result *result)
{
    size_t region_bytes;
    int64_t start, end;
    result->payload_bytes = 0;
    result->elapsed_ns = 0;
    if (bench_validate(c, &region_bytes)) return -1;
    if (run_calls(c->warmup, c->window, ops, context)) return -1;
    /* Re-poison after warmup so warmup alone cannot satisfy final verification. */
    if (ops->reset(context)) return -1;
    start = ops->now_ns(context);
    if (start < 0) return -1;
    if (run_calls(c->iterations, c->window, ops, context)) return -1;
    end = ops->now_ns(context);
    if (end < 0) return -1;
    if (end <= start) { errno = ERANGE; return -1; }
    result->elapsed_ns = end - start;
    result->payload_bytes = (uint64_t)region_bytes * c->iterations;
    return 0;
}
