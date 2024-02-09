#include "test_ubi.h"

void test_bdev_recreate(const char *base_bdev, const char *image_path, int *n_tests,
                        int *n_failures) {
    const char *bdev_name = "test_bdev_recreate_ubi0";

    struct ubi_create_request create_req;
    memset(&create_req, 0, sizeof(create_req));
    create_req.opts.base_bdev_name = base_bdev;
    create_req.opts.image_path = image_path;
    create_req.opts.stripe_size_kb = 1024;
    create_req.opts.name = (char *)bdev_name;

    struct ubi_delete_request delete_req;
    memset(&delete_req, 0, sizeof(delete_req));
    delete_req.name = (char *)bdev_name;

    for (int i = 0; i < 2; i++) {
        execute_app_function(init_thread_create_bdev_ubi, &create_req);
        if (!create_req.success) {
            SPDK_WARNLOG("create_bdev_ubi failed\n");
            (*n_failures)++;
            break;
        }
        test_bdev_io(bdev_name, image_path, n_tests, n_failures);
        execute_app_function(init_thread_delete_bdev_ubi, &delete_req);
        if (!delete_req.success) {
            SPDK_WARNLOG("delete_bdev_ubi failed\n");
            (*n_failures)++;
            break;
        }
    }
}
