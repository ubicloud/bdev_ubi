#include "test_ubi.h"

struct bdev_io_test_state {
    struct test_bdev bdev;
    FILE *image_file;

    uint32_t blocklen;
    uint64_t blockcnt;
    uint64_t image_size;
    uint64_t n_image_blocks;
};

static bool open_bdev(const char *bdev_name, struct bdev_io_test_state *state);
static bool open_base_image(const char *image_path, struct bdev_io_test_state *state);
static bool test_read(struct bdev_io_test_state *state, uint32_t start, uint32_t count);
static bool test_write(struct bdev_io_test_state *state, uint32_t start, uint32_t count);
static bool test_random_ops(struct bdev_io_test_state *state, uint32_t count);
static bool verify_image_block(struct bdev_io_test_state *state, uint64_t block,
                               char *buf);
static bool file_size(FILE *f, uint64_t *out);

void test_bdev_io(const char *bdev_name, const char *image_path, int *n_tests,
                  int *n_failures) {
    struct bdev_io_test_state state;
    memset(&state, 0, sizeof(state));

    if (!open_base_image(image_path, &state)) {
        (*n_failures)++;
        return;
    }

    if (!open_bdev(bdev_name, &state)) {
        fclose(state.image_file);
        (*n_failures)++;
        return;
    }

    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(state.bdev.desc);
    state.blocklen = bdev->blocklen;
    state.blockcnt = bdev->blockcnt;
    state.n_image_blocks = state.image_size / state.blocklen;

#define RUN_TEST(x)                                                                      \
    {                                                                                    \
        (*n_tests)++;                                                                    \
        if (!(x)) {                                                                      \
            (*n_failures)++;                                                             \
        }                                                                                \
    }

    // read 100 blocks from the image addresses
    RUN_TEST(test_read(&state, 10, 100));
    // read 100 blocks from the non-image addresses
    RUN_TEST(test_read(&state, state.n_image_blocks + 2, 100));
    // write 100 blocks to the image addresses
    RUN_TEST(test_write(&state, 20, 100));
    // write 100 blocks to the non-image addresses
    RUN_TEST(test_write(&state, state.n_image_blocks + 2, 100));
    // Some random io
    RUN_TEST(test_random_ops(&state, 50));

    execute_spdk_function(close_io_channel, &state);
    spdk_bdev_close(state.bdev.desc);
}

static bool open_base_image(const char *image_path, struct bdev_io_test_state *state) {
    state->image_file = fopen(image_path, "r");
    if (state->image_file == NULL) {
        int rc = errno;
        SPDK_ERRLOG("Could not open %s: %s\n", image_path, strerror(-rc));
        return false;
    }

    if (!file_size(state->image_file, &state->image_size)) {
        fclose(state->image_file);
        return false;
    }

    return true;
}

static void ubi_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
                         void *event_ctx) {
    SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}

static bool open_bdev(const char *bdev_name, struct bdev_io_test_state *state) {
    int rc = spdk_bdev_open_ext(bdev_name, true, ubi_event_cb, NULL, &state->bdev.desc);
    if (rc < 0) {
        SPDK_ERRLOG("Could not open bdev %s: %s\n", bdev_name, strerror(-rc));
        return false;
    }

    execute_spdk_function(open_io_channel, &state->bdev);
    if (state->bdev.desc == NULL || state->bdev.ch == NULL) {
        spdk_bdev_close(state->bdev.desc);
        return false;
    }

    return true;
}

static bool test_read(struct bdev_io_test_state *state, uint32_t start, uint32_t count) {
    struct ubi_io_request req;

    req.bdev = &state->bdev;
    for (size_t i = 0; i < count; i++) {
        req.block_idx = start + count;
        execute_spdk_function(io_thread_read, &req);
        if (!req.success) {
            return false;
        }

        if (req.block_idx >= state->n_image_blocks)
            continue;

        if (!verify_image_block(state, req.block_idx, req.buf)) {
            return false;
        }
    }

    return true;
}

static bool test_write(struct bdev_io_test_state *state, uint32_t start, uint32_t count) {
    struct ubi_io_request read_req, write_req;

    read_req.bdev = &state->bdev;
    write_req.bdev = &state->bdev;
    for (size_t i = 0; i < count; i++) {
        write_req.block_idx = start + count;
        read_req.block_idx = start + count;

        for (size_t j = 0; j < state->blocklen; j++) {
            write_req.buf[j] = rand() % 128;
        }

        execute_spdk_function(io_thread_write, &write_req);
        if (!write_req.success) {
            return false;
        }

        execute_spdk_function(io_thread_read, &read_req);
        if (!read_req.success) {
            return false;
        }

        if (memcmp(write_req.buf, read_req.buf, state->blocklen)) {
            SPDK_ERRLOG("Read data didn't match written data.\n");
            return false;
        }
    }

    return true;
}

static bool test_random_ops(struct bdev_io_test_state *state, uint32_t count) {
    struct ubi_io_request req;
    req.bdev = &state->bdev;
    for (size_t i = 0; i < count; i++) {
        int type = rand() % 3;
        req.block_idx = rand() % state->blockcnt;
        switch (type) {
        case 0:
            execute_spdk_function(io_thread_read, &req);
            break;
        case 1:
            execute_spdk_function(io_thread_write, &req);
            break;
        case 2:
            execute_spdk_function(io_thread_flush, &req);
            break;
        }
        if (!req.success)
            return false;
    }

    return true;
}

static bool verify_image_block(struct bdev_io_test_state *state, uint64_t block,
                               char *buf) {
    char image_buf[MAX_BLOCK_SIZE];

    int rc = fseek(state->image_file, block * state->blocklen, SEEK_SET);
    if (rc) {
        SPDK_ERRLOG("fseek failed: %s\n", strerror(errno));
        return false;
    }

    size_t len = fread(image_buf, 1, state->blocklen, state->image_file);
    if (len != state->blocklen) {
        SPDK_ERRLOG("fread failed, expected %u bytes, read %lu bytes\n", state->blocklen,
                    len);
        return false;
    }

    if (memcmp(image_buf, buf, state->blocklen)) {
        SPDK_ERRLOG("memcmp failed.\n");
        return false;
    }

    return true;
}

static bool file_size(FILE *f, uint64_t *out) {
    int rc = fseek(f, 0, SEEK_END);
    if (rc) {
        SPDK_ERRLOG("fseek failed: %s\n", strerror(errno));
        return false;
    }

    *out = ftell(f);
    return true;
}
