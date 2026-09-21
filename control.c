#define _GNU_SOURCE
#include "control.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int parse_number(const char *text, uint64_t min, uint64_t max, uint64_t *out)
{
    unsigned long long value;
    if (!text || !*text || strspn(text, "0123456789") != strlen(text)) { errno = EINVAL; return -1; }
    errno = 0;
    value = strtoull(text, NULL, 10);
    if (errno || value < min || value > max) { errno = ERANGE; return -1; }
    *out = (uint64_t)value;
    return 0;
}

int valid_ip(const char *ip)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1 || addr.s_addr == htonl(INADDR_ANY) ||
        addr.s_addr == htonl(INADDR_BROADCAST)) { errno = EINVAL; return 0; }
    return 1;
}

void control_init(struct control *c, int timeout_ms) { c->fd = -1; c->deadline_ms = monotonic_ms() + timeout_ms; }
void control_close(struct control *c) { if (c->fd >= 0) close(c->fd); c->fd = -1; }

static int wait_fd(struct control *c, int fd, short events)
{
    for (;;) {
        int64_t remaining = c->deadline_ms - monotonic_ms();
        struct pollfd pfd;
        int rc;
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fd;
        pfd.events = events;
        rc = poll(&pfd, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (rc < 0 && errno == EINTR) continue;
        if (!rc) errno = ETIMEDOUT;
        if (rc <= 0) return -1;
        if (pfd.revents & POLLNVAL) { errno = EBADF; return -1; }
        return 0; /* recv/send/SO_ERROR reports hangup and errors precisely. */
    }
}

static int address_of(const char *ip, uint16_t port, struct sockaddr_in *addr)
{
    if (!valid_ip(ip) || !port) { errno = EINVAL; return -1; }
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    return inet_pton(AF_INET, ip, &addr->sin_addr) == 1 ? 0 : -1;
}

int control_accept(struct control *c, const char *ip, uint16_t port)
{
    struct sockaddr_in addr;
    int listener, one = 1, result = -1, saved;
    if (address_of(ip, port, &addr)) return -1;
    listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener < 0) return -1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 1)) goto out;
    for (;;) {
        if (wait_fd(c, listener, POLLIN)) break;
        c->fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c->fd >= 0) { result = 0; break; }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) break;
    }
out:
    saved = errno;
    close(listener);
    errno = saved;
    return result;
}

int control_connect(struct control *c, const char *ip, uint16_t port)
{
    struct sockaddr_in addr;
    if (address_of(ip, port, &addr)) return -1;
    for (;;) {
        int error = 0;
        struct timespec pause = {0, 20000000};
        if (monotonic_ms() >= c->deadline_ms) { errno = ETIMEDOUT; return -1; }
        c->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (c->fd < 0) return -1;
        if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr))) {
            error = errno;
            if (error == EINPROGRESS) {
                if (wait_fd(c, c->fd, POLLOUT)) error = errno;
                else {
                    socklen_t len = sizeof(error);
                    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &len)) error = errno;
                }
            }
        }
        if (!error) return 0;
        control_close(c);
        if (error != ECONNREFUSED && error != EINTR) { errno = error; return -1; }
        nanosleep(&pause, NULL);
    }
}

int control_send(struct control *c, const char *message)
{
    char frame[CONTROL_FRAME_SIZE] = {0};
    size_t offset = 0;
    if (strlen(message) >= sizeof(frame)) { errno = EMSGSIZE; return -1; }
    memcpy(frame, message, strlen(message));
    while (offset < sizeof(frame)) {
        ssize_t n;
        if (wait_fd(c, c->fd, POLLOUT)) return -1;
        n = send(c->fd, frame + offset, sizeof(frame) - offset, MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) { if (!n) errno = EPIPE; return -1; }
        offset += (size_t)n;
    }
    return 0;
}

int control_receive(struct control *c, char message[CONTROL_FRAME_SIZE])
{
    size_t offset = 0, end, i;
    while (offset < CONTROL_FRAME_SIZE) {
        ssize_t n;
        if (wait_fd(c, c->fd, POLLIN)) return -1;
        n = recv(c->fd, message + offset, CONTROL_FRAME_SIZE - offset, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
        offset += (size_t)n;
    }
    end = strnlen(message, CONTROL_FRAME_SIZE);
    if (end == CONTROL_FRAME_SIZE) { errno = EPROTO; return -1; }
    for (i = end; i < CONTROL_FRAME_SIZE; ++i) {
        if (message[i]) { errno = EPROTO; return -1; }
    }
    return 0;
}

int metadata_encode(const struct metadata *m, char out[CONTROL_FRAME_SIZE])
{
    int n = snprintf(out, CONTROL_FRAME_SIZE, "A2ROCE1 %" PRIu64 " %" PRIu64 " %" PRIu64
                     " %" PRIu64 " %" PRIu64 " %s %s", m->kind, m->bytes, m->address,
                     m->physical_device, m->rank, m->host_ip, m->npu_ip);
    if (n < 0 || n >= CONTROL_FRAME_SIZE) { errno = EMSGSIZE; return -1; }
    return 0;
}

int metadata_decode(const char *text, struct metadata *m)
{
    char copy[CONTROL_FRAME_SIZE], *save = NULL, *tokens[9] = {0}, *s;
    size_t n = 0;
    if (strlen(text) >= sizeof(copy)) { errno = EPROTO; return -1; }
    strcpy(copy, text);
    for (s = strtok_r(copy, " ", &save); s && n < 9; s = strtok_r(NULL, " ", &save)) tokens[n++] = s;
    if (n != 8 || strcmp(tokens[0], "A2ROCE1") ||
        parse_number(tokens[1], 0, 1, &m->kind) || parse_number(tokens[2], 1, SIZE_MAX, &m->bytes) ||
        parse_number(tokens[3], 1, UINTPTR_MAX, &m->address) ||
        parse_number(tokens[4], 0, INT32_MAX, &m->physical_device) || parse_number(tokens[5], 0, 1, &m->rank) ||
        strlen(tokens[6]) >= sizeof(m->host_ip) || strlen(tokens[7]) >= sizeof(m->npu_ip) ||
        !valid_ip(tokens[6]) || !valid_ip(tokens[7]) || m->address > UINTPTR_MAX - m->bytes) {
        errno = EPROTO; return -1;
    }
    strcpy(m->host_ip, tokens[6]);
    strcpy(m->npu_ip, tokens[7]);
    return 0;
}

int metadata_validate(const struct metadata *local, const struct metadata *peer, const char *peer_host)
{
    if (local->kind != peer->kind || local->bytes != peer->bytes || local->rank == peer->rank ||
        strcmp(peer->host_ip, peer_host) || !strcmp(local->npu_ip, peer->npu_ip) ||
        (!strcmp(local->host_ip, peer->host_ip) && local->physical_device == peer->physical_device)) {
        errno = EPROTO; return -1;
    }
    return 0;
}

int make_rank_table(const struct metadata *source, const struct metadata *target, char *out, size_t capacity)
{
    char dev0[160], dev1[160];
    int n;
    snprintf(dev0, sizeof(dev0), "{\"device_id\":\"%" PRIu64 "\",\"device_ip\":\"%s\",\"rank_id\":\"0\"}",
             source->physical_device, source->npu_ip);
    snprintf(dev1, sizeof(dev1), "{\"device_id\":\"%" PRIu64 "\",\"device_ip\":\"%s\",\"rank_id\":\"1\"}",
             target->physical_device, target->npu_ip);
    if (!strcmp(source->host_ip, target->host_ip)) {
        n = snprintf(out, capacity, "{\"version\":\"1.0\",\"status\":\"completed\",\"server_count\":\"1\","
                     "\"server_list\":[{\"server_id\":\"%s\",\"device\":[%s,%s]}]}", source->host_ip, dev0, dev1);
    } else {
        n = snprintf(out, capacity, "{\"version\":\"1.0\",\"status\":\"completed\",\"server_count\":\"2\","
                     "\"server_list\":[{\"server_id\":\"%s\",\"device\":[%s]},"
                     "{\"server_id\":\"%s\",\"device\":[%s]}]}", source->host_ip, dev0, target->host_ip, dev1);
    }
    if (n < 0 || (size_t)n >= capacity) { errno = EMSGSIZE; return -1; }
    return 0;
}
