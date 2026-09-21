#ifndef A2_ROCE_CONTROL_H
#define A2_ROCE_CONTROL_H
#include <stddef.h>
#include <stdint.h>
#define CONTROL_FRAME_SIZE 256
struct control { int fd; int64_t deadline_ms; };
struct metadata {
    uint64_t kind, bytes, address, physical_device, rank;
    char host_ip[16], npu_ip[16];
};
int64_t monotonic_ms(void);
int parse_number(const char *text, uint64_t min, uint64_t max, uint64_t *out);
int valid_ip(const char *ip);
void control_init(struct control *control, int timeout_ms);
void control_close(struct control *control);
int control_accept(struct control *control, const char *ip, uint16_t port);
int control_connect(struct control *control, const char *ip, uint16_t port);
int control_send(struct control *control, const char *message);
int control_receive(struct control *control, char message[CONTROL_FRAME_SIZE]);
int metadata_encode(const struct metadata *m, char out[CONTROL_FRAME_SIZE]);
int metadata_decode(const char *text, struct metadata *m);
int metadata_validate(const struct metadata *local, const struct metadata *peer, const char *peer_host);
int make_rank_table(const struct metadata *source, const struct metadata *target, char *out, size_t capacity);
#endif
