

#include "test_ubi.h"

bool verify_create(const char *base_bdev, const char *image_path, const char *bdev_name) {
    struct ubi_create_request create_req;
    memset(&create_req, 0, sizeof(create_req));
    create_req.opts.base_bdev_name = base_bdev;
    create_req.opts.image_path = image_path;
    create_req.opts.stripe_size_kb = 1024;
    create_req.opts.name = (char *)bdev_name;

    execute_app_function(init_thread_create_bdev_ubi, &create_req);
    if (!create_req.success) {
        SPDK_WARNLOG(
            "create_bdev_ubi failed for base_bdev: %s, image_path: %s, bdev_name: %s\n",
            base_bdev, image_path, bdev_name);
        return false;
    }

    return true;
}

bool verify_delete(const char *bdev_name) {
    struct ubi_delete_request delete_req;
    memset(&delete_req, 0, sizeof(delete_req));
    delete_req.name = (char *)bdev_name;

    execute_app_function(init_thread_delete_bdev_ubi, &delete_req);
    if (!delete_req.success) {
        SPDK_WARNLOG("delete_bdev_ubi failed for bdev_name: %s\n", bdev_name);
        return false;
    }

    return true;
}

static void ubi_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
                         void *event_ctx) {
    SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}

bool open_bdev_and_ch(const char *bdev_name, struct bdev_desc_ch_pair *bdev) {
    int rc = spdk_bdev_open_ext(bdev_name, true, ubi_event_cb, NULL, &bdev->desc);
    if (rc < 0) {
        SPDK_ERRLOG("Could not open bdev %s: %s\n", bdev_name, strerror(-rc));
        return false;
    }

    execute_spdk_function(open_io_channel, bdev);
    if (bdev->ch == NULL) {
        spdk_bdev_close(bdev->desc);
        bdev->desc = NULL;
        return false;
    }

    return true;
}

bool close_bdev_and_ch(struct bdev_desc_ch_pair *bdev) {
    execute_spdk_function(close_io_channel, bdev);
    if (bdev->desc) {
        spdk_bdev_close(bdev->desc);
    }
    return true;
}
