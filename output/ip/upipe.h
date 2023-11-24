#ifndef OUTPUT_IP_UPIPE_H
#define OUTPUT_IP_UPIPE_H 1
struct uref_mgr;
struct ubuf_mgr;
struct uclock;

struct uref_ctx {
    struct uref_mgr *uref_mgr;
    struct ubuf_mgr *ubuf_mgr;
    struct uclock *uclock;
};

struct uref *make_uref(struct uref_ctx *ctx, uint8_t *buf, size_t len,
        int64_t timestamp);
#endif
