#include "bdev_ubi_test_control.h"
#include "test_ubi.h"

static bool do_test_io_channel_create_errors(const char *bdev_name);

bool test_io_channel_create_errors(void) {
    const char *bdev_name = "test_io_channel_create_error_ubi0";
    const char *base_bdev = TEST_FREE_BASE_BDEV;
    const char *image_path = TEST_IMAGE_PATH;

    if (!verify_create(base_bdev, image_path, bdev_name)) {
        SPDK_WARNLOG("Initial create & delete verification failed\n");
        return false;
    }

    bool success = do_test_io_channel_create_errors(bdev_name);

    if (!verify_delete(bdev_name)) {
        SPDK_WARNLOG("Failed to delete bdev UBI: %s\n", bdev_name);
        return false;
    }

    return success;
}

static bool do_test_io_channel_create_errors(const char *bdev_name) {
    // It should normally succeed
    {
        struct bdev_desc_ch_pair desc_ch_pair = {0};
        if (!open_bdev_and_ch(bdev_name, &desc_ch_pair) ||
            !close_bdev_and_ch(&desc_ch_pair)) {
            SPDK_WARNLOG("Failed to open or close bdev UBI: %s\n", bdev_name);
            return false;
        }
    }

    // Case where we cannot register a poller
    {
        bool success = true;
        struct bdev_desc_ch_pair desc_ch_pair = {0};
        ubi_io_channel_fail_register_poller(true);
        if (open_bdev_and_ch(bdev_name, &desc_ch_pair)) {
            SPDK_WARNLOG("open_bdev_and_ch succeeded when poller registration failed\n");
            success = false;
        }
        ubi_io_channel_fail_register_poller(false);
        close_bdev_and_ch(&desc_ch_pair);
        if (!success) {
            return false;
        }
    }

    // Case where we cannot get base io channel
    {
        bool success = true;
        struct bdev_desc_ch_pair desc_ch_pair = {0};
        ubi_io_channel_fail_create_base_ch(true);
        if (open_bdev_and_ch(bdev_name, &desc_ch_pair)) {
            SPDK_WARNLOG(
                "open_bdev_and_ch succeeded when getting base io channel failed\n");
            success = false;
        }
        ubi_io_channel_fail_create_base_ch(false);
        close_bdev_and_ch(&desc_ch_pair);
        if (!success) {
            return false;
        }
    }

    // Case where we cannot open image file
    {
        bool success = true;
        struct bdev_desc_ch_pair desc_ch_pair = {0};
        ubi_io_channel_fail_image_file_open(true);
        if (open_bdev_and_ch(bdev_name, &desc_ch_pair)) {
            SPDK_WARNLOG("open_bdev_and_ch succeeded when opening image file failed\n");
            success = false;
        }
        ubi_io_channel_fail_image_file_open(false);
        close_bdev_and_ch(&desc_ch_pair);
        if (!success) {
            return false;
        }
    }

    // Case where we cannot initialize io_uring
    {
        bool success = true;
        struct bdev_desc_ch_pair desc_ch_pair = {0};
        ubi_io_channel_fail_uring_queue_init(true);
        if (open_bdev_and_ch(bdev_name, &desc_ch_pair)) {
            SPDK_WARNLOG(
                "open_bdev_and_ch succeeded when initializing io_uring failed\n");
            success = false;
        }
        ubi_io_channel_fail_uring_queue_init(false);
        close_bdev_and_ch(&desc_ch_pair);
        if (!success) {
            return false;
        }
    }

    return true;
}
