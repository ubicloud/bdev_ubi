#include "test_ubi.h"

void exit_io_thread(void *arg) {
    spdk_thread_exit(spdk_get_thread());
    wake_ut_thread();
}

void open_io_channel(void *arg) {
    struct test_state *state = arg;

    state->ch = spdk_bdev_get_io_channel(state->bdev_desc);
    if (state->ch == NULL) {
        SPDK_ERRLOG("Could not get I/O channel: %s\n", strerror(ENOMEM));
    }

    wake_ut_thread();
}

void close_io_channel(void *arg) {
    struct test_state *state = arg;
    if (state->ch)
        spdk_put_io_channel(state->ch);

    wake_ut_thread();
}

static void io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg) {
    struct ubi_io_request *req = arg;
    req->success = success;
    spdk_bdev_free_io(bdev_io);
    wake_ut_thread();
}

void ubi_bdev_write(void *arg) {
    struct ubi_io_request *req = arg;

    // Reset success. This will be set in the completion callback.
    req->success = false;

    int rc = spdk_bdev_write_blocks(req->state->bdev_desc, req->state->ch, req->buf,
                                    req->block_idx, 1, io_completion_cb, req);

    if (rc) {
        wake_ut_thread();
    }
}

void ubi_bdev_read(void *arg) {
    struct ubi_io_request *req = arg;

    // Reset success. This will be set in the completion callback.
    req->success = false;

    int rc = spdk_bdev_read_blocks(req->state->bdev_desc, req->state->ch, req->buf,
                                   req->block_idx, 1, io_completion_cb, req);

    if (rc) {
        wake_ut_thread();
    }
}

void ubi_bdev_flush(void *arg) {
    struct ubi_io_request *req = arg;

    // Reset success. This will be set in the completion callback.
    req->success = false;

    int rc = spdk_bdev_flush_blocks(req->state->bdev_desc, req->state->ch, req->block_idx,
                                    1, io_completion_cb, req);

    if (rc) {
        wake_ut_thread();
    }
}
