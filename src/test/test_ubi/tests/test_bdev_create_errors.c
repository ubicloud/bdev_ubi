#include "bdev_ubi_test_control.h"
#include "test_ubi.h"

bool test_bdev_create_errors(void) {
    const char *bdev_name = "test_bdev_create_error_ubi0";
    const char *base_bdev = TEST_FREE_BASE_BDEV;
    const char *image_path = TEST_IMAGE_PATH;
    const char *base_with_invalid_magic = TEST_BASE_WITH_INVALID_MAGIC;
    const char *too_small_base = TEST_TOO_SMALL_BASE;

    struct ubi_create_request create_req;
    memset(&create_req, 0, sizeof(create_req));
    create_req.opts.base_bdev_name = base_bdev;
    create_req.opts.image_path = image_path;
    create_req.opts.stripe_size_kb = 1024;
    create_req.opts.name = (char *)bdev_name;

    // Normally, it should succeed
    if (!verify_create(base_bdev, image_path, bdev_name) || !verify_delete(bdev_name)) {
        SPDK_WARNLOG("Initial create & delete verification failed\n");
        return false;
    }

    // Case where we cannot create a channel to read metadata
    ubi_fail_create_channel_for_metadata_read(true);
    execute_app_function(init_thread_create_bdev_ubi, &create_req);
    ubi_fail_create_channel_for_metadata_read(false);
    if (create_req.success) {
        SPDK_WARNLOG("create_bdev_ubi succeeded when channel creation failed\n");
        return false;
    }

    // Case where we cannot allocate memory for ubi_bdev
    ubi_fail_calloc_ubi_bdev(true);
    execute_app_function(init_thread_create_bdev_ubi, &create_req);
    ubi_fail_calloc_ubi_bdev(false);
    if (create_req.success) {
        SPDK_WARNLOG("create_bdev_ubi succeeded when memory allocation failed\n");
        return false;
    }

    // Case where base bdev has invalid magic
    create_req.opts.base_bdev_name = base_with_invalid_magic;
    execute_app_function(init_thread_create_bdev_ubi, &create_req);
    create_req.opts.base_bdev_name = base_bdev;
    if (create_req.success) {
        SPDK_WARNLOG("create_bdev_ubi succeeded with invalid magic base bdev\n");
        return false;
    }

    // Case where base bdev is too small
    create_req.opts.base_bdev_name = too_small_base;
    execute_app_function(init_thread_create_bdev_ubi, &create_req);
    create_req.opts.base_bdev_name = base_bdev;
    if (create_req.success) {
        SPDK_WARNLOG("create_bdev_ubi succeeded with too small base bdev\n");
        return false;
    }

    // Case where we cannot duplicate the name for ubi_bdev
    create_req.opts.name = NULL;
    execute_app_function(init_thread_create_bdev_ubi, &create_req);
    create_req.opts.name = (char *)bdev_name;
    if (create_req.success) {
        SPDK_WARNLOG("create_bdev_ubi succeeded with NULL name\n");
        return false;
    }

    // Case with invalid image path
    create_req.opts.image_path = "/invalid/path";
    execute_app_function(init_thread_create_bdev_ubi, &create_req);
    create_req.opts.image_path = image_path;
    if (create_req.success) {
        SPDK_WARNLOG("create_bdev_ubi succeeded with invalid image path\n");
        return false;
    }

    // Case where stripe size is too small
    create_req.opts.stripe_size_kb = 1;
    execute_app_function(init_thread_create_bdev_ubi, &create_req);
    create_req.opts.stripe_size_kb = 1024;
    if (create_req.success) {
        SPDK_WARNLOG("create_bdev_ubi succeeded with too small stripe size\n");
        return false;
    }

    // Case where stripe size is not a power of 2
    create_req.opts.stripe_size_kb = 215;
    execute_app_function(init_thread_create_bdev_ubi, &create_req);
    create_req.opts.stripe_size_kb = 1024;
    if (create_req.success) {
        SPDK_WARNLOG("create_bdev_ubi succeeded with non-power-of-2 stripe size\n");
        return false;
    }

    // Case for registering twice with the same name
    if (!verify_create(base_bdev, image_path, bdev_name) ||
        verify_create("free_base_bdev_2", image_path, bdev_name) ||
        !verify_delete(bdev_name)) {
        SPDK_WARNLOG("Create verification failed for duplicate name registration\n");
        return false;
    }

    // Case for registering twice with the same base bdev
    if (!verify_create(base_bdev, image_path, bdev_name) ||
        verify_create(base_bdev, image_path, "test_bdev_create_error_ubi1") ||
        !verify_delete(bdev_name)) {
        SPDK_WARNLOG("Create verification failed for duplicate base bdev registration\n");
        return false;
    }

    // Finally check again if we can create and delete. This is to check
    // (to some degree) that the previous failure handling cases did
    // proper cleanup.
    if (!verify_create(base_bdev, image_path, bdev_name) || !verify_delete(bdev_name)) {
        SPDK_WARNLOG("Final create & delete verification failed\n");
        return false;
    }

    return true;
}
