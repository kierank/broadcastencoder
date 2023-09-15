#include "udp.h"
#include "upipe.h"

struct upipe;
struct uprobe;
struct uqueue;
struct uref_mgr;
struct ubuf_mgr;
struct ueventfd;
struct uclock;

struct srt_ctx {
    pthread_t thread;
    struct uref_ctx *uref_ctx;

    struct upipe *upipe_setflowdef;
    struct upipe *upipe_udpsink;
    struct upipe *upipe_udpsrc_srt;
    struct upipe *upipe_srt_handshake_sub;
    struct upipe *upipe_srt_sender;
    struct upipe *upipe_srt_sender_sub;

    struct uprobe *uprobe_udp_srt;
    struct uprobe *logger;

    struct upump_mgr *upump_mgr;

    struct ueventfd *event;

    // XXX
    bool restart;

    // password
    int fd;
    struct sockaddr_storage dest_addr;
    int dest_addr_len;
    unsigned latency;

    pthread_mutex_t mutex;
    bool end;
    struct uqueue *queue;
    void *queue_extra;
};

void srt_write(struct srt_ctx *ctx, struct uref *uref);
struct srt_ctx *open_srt(obe_udp_ctx *p_udp, unsigned latency);
void close_srt(struct srt_ctx *ctx);
int srt_bidirectional(struct srt_ctx *ctx);

