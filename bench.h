#ifndef A2_ROCE_BENCH_H
#define A2_ROCE_BENCH_H
#include <stddef.h>
#include <stdint.h>

struct bench_config {
    size_t message_bytes;
    uint32_t batch, warmup, iterations, window;
};
struct bench_result { uint64_t payload_bytes; int64_t elapsed_ns; };
struct bench_ops {
    int (*submit)(void *context);
    int (*sync)(void *context);
    int (*reset)(void *context);
    int64_t (*now_ns)(void *context);
};
int bench_validate(const struct bench_config *config, size_t *region_bytes);
int bench_run(const struct bench_config *config, const struct bench_ops *ops,
              void *context, struct bench_result *result);
#endif
