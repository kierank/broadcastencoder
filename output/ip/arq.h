#include "udp.h"

struct upipe;
struct uqueue;
struct uref_mgr;
struct ubuf_mgr;
struct ueventfd;
struct uclock;

struct arq_ctx {
    pthread_t thread;
    struct upipe *upipe_udpsink;
    struct upipe *upipe_rtcpfb;
    struct upipe *upipe_udpsrc;
    struct upipe *upipe_rtcp_sub;
    struct uref_mgr *uref_mgr;
    struct ubuf_mgr *ubuf_mgr;
    struct ueventfd *event;
    struct uclock *uclock;
    uint64_t last_sr_ntp;
    uint64_t last_sr_cr;
    int fd;
    struct sockaddr_storage dest_addr;
    int dest_addr_len;
    unsigned latency;
    uint8_t rtx_pt;

    pthread_mutex_t mutex;
    bool end;
    struct uqueue *queue;
    void *queue_extra;
};

void arq_write(struct arq_ctx *ctx, uint8_t *buf, size_t len, int64_t timestamp);
struct arq_ctx *open_arq(obe_udp_ctx *p_udp, unsigned latency, uint8_t rtx_pt);
void close_arq(struct arq_ctx *ctx);
