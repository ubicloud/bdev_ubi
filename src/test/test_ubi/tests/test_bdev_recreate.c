#include "test_ubi.h"

bool test_bdev_recreate(void) {
    const char *bdev_name = "test_bdev_recreate_ubi0";
    const char *image_path = TEST_IMAGE_PATH;

    for (int i = 0; i < 2; i++) {
        if (!verify_create(TEST_FREE_BASE_BDEV, image_path, bdev_name)) {
            SPDK_WARNLOG("Failed to create bdev UBI: %s\n", bdev_name);
            return false;
        }

        int n_io_tests = 0, n_io_failures = 0;
        test_bdev_io(bdev_name, &n_io_tests, &n_io_failures);

        if (!verify_delete(bdev_name)) {
            SPDK_WARNLOG("Failed to delete bdev UBI: %s\n", bdev_name);
            return false;
        }

        if (n_io_failures > 0) {
            SPDK_WARNLOG("Failed %d I/O tests\n", n_io_failures);
            return false;
        }
    }
    return true;
}
