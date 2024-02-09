#include "bdev_ubi.h"
#include "test_ubi.h"

void exit_io_thread(void *arg) {
    spdk_thread_exit(spdk_get_thread());
    wake_ut_thread();
}

void open_io_channel(void *arg) {
    struct test_bdev *bdev = arg;

    bdev->ch = spdk_bdev_get_io_channel(bdev->desc);
    if (bdev->ch == NULL) {
        SPDK_ERRLOG("Could not get I/O channel: %s\n", strerror(ENOMEM));
    }

    wake_ut_thread();
}

void close_io_channel(void *arg) {
    struct test_bdev *bdev = arg;
    if (bdev->ch)
        spdk_put_io_channel(bdev->ch);

    wake_ut_thread();
}

static void io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg) {
    struct ubi_io_request *req = arg;
    req->success = success;
    spdk_bdev_free_io(bdev_io);
    wake_ut_thread();
}

void io_thread_write(void *arg) {
    struct ubi_io_request *req = arg;

    // Reset success. This will be set in the completion callback.
    req->success = false;

    int rc = spdk_bdev_write_blocks(req->bdev->desc, req->bdev->ch, req->buf,
                                    req->block_idx, 1, io_completion_cb, req);

    if (rc) {
        wake_ut_thread();
    }
}

void io_thread_read(void *arg) {
    struct ubi_io_request *req = arg;

    // Reset success. This will be set in the completion callback.
    req->success = false;

    int rc = spdk_bdev_read_blocks(req->bdev->desc, req->bdev->ch, req->buf,
                                   req->block_idx, 1, io_completion_cb, req);

    if (rc) {
        wake_ut_thread();
    }
}

void io_thread_flush(void *arg) {
    struct ubi_io_request *req = arg;

    // Reset success. This will be set in the completion callback.
    req->success = false;

    int rc = spdk_bdev_flush_blocks(req->bdev->desc, req->bdev->ch, req->block_idx, 1,
                                    io_completion_cb, req);

    if (rc) {
        wake_ut_thread();
    }
}
