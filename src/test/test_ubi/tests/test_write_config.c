#include "bdev_ubi_internal.h"
#include "test_ubi.h"

static char output[4096];
static int output_len = 0;
const char *expected_output = "{"
                              "\"method\":\"bdev_ubi_create\","
                              "\"params\":{"
                              "\"name\":\"test_bdev_write_config_ubi0\","
                              "\"base_bdev\":\"free_base_bdev\","
                              "\"image_path\":\"bin/test/test_image.raw\","
                              "\"stripe_size_kb\":1024,"
                              "\"copy_on_read\":false,"
                              "\"directio\":false,"
                              "\"no_sync\":false"
                              "}"
                              "}";

static int write_cb(void *cb_ctx, const void *data, size_t size) {
    if (output_len + size > sizeof(output)) {
        return -1;
    }
    memcpy(output + output_len, data, size);
    output_len += size;
    return 0;
}

static void call_write_config_json(void *name) {
    struct spdk_json_write_ctx *w = spdk_json_write_begin(write_cb, NULL, 0);
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(name);
    ubi_write_config_json(bdev, w);
    spdk_json_write_end(w);
    output[output_len] = '\0';

    wake_ut_thread();
}

bool test_write_config(void) {
    const char *bdev_name = "test_bdev_write_config_ubi0";
    const char *base_bdev = TEST_FREE_BASE_BDEV;
    const char *image_path = TEST_IMAGE_PATH;

    struct ubi_create_request create_req;
    memset(&create_req, 0, sizeof(create_req));
    create_req.opts.base_bdev_name = base_bdev;
    create_req.opts.image_path = image_path;
    create_req.opts.stripe_size_kb = 1024;
    create_req.opts.name = (char *)bdev_name;
    execute_app_function(init_thread_create_bdev_ubi, &create_req);
    if (!create_req.success) {
        SPDK_WARNLOG("create_bdev_ubi failed\n");
        return false;
    }

    bool success = true;
    execute_app_function(call_write_config_json, (void *)bdev_name);
    if (strcmp(output, expected_output)) {
        SPDK_WARNLOG("output: %s\n", output);
        SPDK_WARNLOG("expected_output: %s\n", expected_output);
        success = false;
    }

    struct ubi_delete_request delete_req;
    memset(&delete_req, 0, sizeof(delete_req));
    delete_req.name = (char *)bdev_name;
    execute_app_function(init_thread_delete_bdev_ubi, &delete_req);
    if (!delete_req.success) {
        SPDK_WARNLOG("delete_bdev_ubi failed\n");
        success = false;
    }

    return success;
}
