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

    struct uprobe *uprobe_udp_srt;
    struct uprobe *logger;

    struct upump_mgr *upump_mgr;

    struct ueventfd *event;

    bool restart;
    unsigned n;

    char *password;
    char *stream_id;
    int fd;
    struct sockaddr_storage dest_addr;
    int dest_addr_len;
    unsigned latency;

    bool listener;

    uint64_t last_ack_cr;

    pthread_mutex_t mutex;
    bool end;
    struct uqueue *queue;
    void *queue_extra;
};

void srt_write(struct srt_ctx *ctx, struct uref *uref);
struct srt_ctx *open_srt(obe_udp_ctx *p_udp, unsigned latency, char *password, char *stream_id, struct uref_ctx *uref_ctx);
void close_srt(struct srt_ctx *ctx);
int srt_bidirectional(struct srt_ctx *ctx);

