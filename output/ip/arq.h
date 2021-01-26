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
    struct upipe *upipe_udpsink_rtcp;
    struct upipe *upipe_row_udpsink;
    struct upipe *upipe_col_udpsink;
    struct upipe *upipe_rtcpfb;
    struct upipe *upipe_udpsrc;
    struct upipe *upipe_rtcp_sub;
    struct uref_mgr *uref_mgr;
    struct ubuf_mgr *ubuf_mgr;
    struct ueventfd *event;
    struct uclock *uclock;
    uint64_t last_sr_ntp;
    uint64_t last_sr_cr;
    uint64_t last_rr_cr;
    int fd;
    struct sockaddr_storage dest_addr;
    int dest_addr_len;
    unsigned latency;
    int row_fd;
    struct sockaddr_storage row_dest_addr;
    int row_dest_addr_len;
    int col_fd;
    struct sockaddr_storage col_dest_addr;
    int col_dest_addr_len;
    int rtcp_fd;
    struct sockaddr_storage rtcp_dest_addr;
    int rtcp_dest_addr_len;

    pthread_mutex_t mutex;
    bool end;
    struct uqueue *queue;
    void *queue_extra;
};

void arq_write(struct arq_ctx *ctx, struct uref *uref);
struct arq_ctx *open_arq(obe_udp_ctx *p_udp, obe_udp_ctx *p_row,
        obe_udp_ctx *p_col, obe_udp_ctx *p_rtcp, unsigned latency);
void close_arq(struct arq_ctx *ctx);
struct uref *make_uref(struct arq_ctx *ctx, uint8_t *buf, size_t len,
        int64_t timestamp);
int arq_bidirectional(struct arq_ctx *ctx);

