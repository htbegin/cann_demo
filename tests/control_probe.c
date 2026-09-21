#include "control.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct control c;
    struct metadata local, peer;
    char message[CONTROL_FRAME_SIZE], table[1024];
    uint64_t port;
    int rc = 1;
    if (argc == 3 && !strcmp(argv[1], "decode")) {
        if (metadata_decode(argv[2], &peer)) return 1;
        return metadata_encode(&peer, message) ? 1 : (puts(message), 0);
    }
    if (argc == 4 && !strcmp(argv[1], "table")) {
        if (metadata_decode(argv[2], &local) || metadata_decode(argv[3], &peer) ||
            metadata_validate(&local, &peer, peer.host_ip) ||
            make_rank_table(&local, &peer, table, sizeof(table))) return 1;
        puts(table);
        return 0;
    }
    if (argc != 3 || parse_number(argv[2], 1, 65535, &port)) return 1;
    control_init(&c, 500);
    if (!strcmp(argv[1], "client")) {
        if (control_connect(&c, "127.0.0.1", (uint16_t)port)) goto out;
    } else if (!strcmp(argv[1], "server")) {
        if (control_accept(&c, "127.0.0.1", (uint16_t)port)) goto out;
    } else goto out;
    if (control_receive(&c, message)) goto out;
    if (strcmp(message, "HELLO") || control_send(&c, "READY")) goto out;
    if (control_receive(&c, message)) goto out;
    if (strcmp(message, "PASS") || control_send(&c, "CLOSED")) goto out;
    rc = 0;
out:
    if (rc) fprintf(stderr, "protocol error: %s\n", strerror(errno));
    control_close(&c);
    return rc;
}
