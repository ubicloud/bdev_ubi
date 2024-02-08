#include "test_ubi.h"

#define IMAGE_PATH "bin/test/test_image.raw"

pthread_mutex_t g_test_mutex;
pthread_cond_t g_test_cond;
struct spdk_thread *g_io_thread;

static void run_bdev_tests(struct test_state *state, const char *name);
static bool test_read(struct test_state *state, uint32_t start, uint32_t count);
static bool test_write(struct test_state *state, uint32_t start, uint32_t count);
static bool test_random_ops(struct test_state *state, uint32_t count);
static bool verify_image_block(struct test_state *state, uint64_t block, char *buf);
static bool file_size(FILE *f, uint64_t *out);

static void execute_spdk_function(spdk_msg_fn fn, void *arg) {
    pthread_mutex_lock(&g_test_mutex);
    spdk_thread_send_msg(g_io_thread, fn, arg);
    pthread_cond_wait(&g_test_cond, &g_test_mutex);
    pthread_mutex_unlock(&g_test_mutex);
}

void wake_ut_thread(void) {
    pthread_mutex_lock(&g_test_mutex);
    pthread_cond_signal(&g_test_cond);
    pthread_mutex_unlock(&g_test_mutex);
}

static void ubi_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
                         void *event_ctx) {
    SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}

void run_ut_thread(void *arg) {
    int rc = 0;

    struct test_opts *opts = arg;
    g_io_thread = opts->io_thread;

    pthread_mutex_init(&g_test_mutex, NULL);
    pthread_cond_init(&g_test_cond, NULL);

    struct test_state state;
    memset(&state, 0, sizeof(state));

    state.image_file = fopen(IMAGE_PATH, "r");
    if (state.image_file == NULL) {
        rc = errno;
        SPDK_ERRLOG("Could not open %s: %s\n", IMAGE_PATH, strerror(-rc));
        state.n_failures++;
        goto cleanup;
    }

    if (!file_size(state.image_file, &state.image_size)) {
        state.n_failures++;
        goto cleanup;
    }

    for (size_t i = 0; i < opts->n_bdevs; i++) {
        SPDK_NOTICELOG("Testing %s\n", opts->bdev_names[i]);
        run_bdev_tests(&state, opts->bdev_names[i]);
    }

cleanup:
    if (state.image_file)
        fclose(state.image_file);

    SPDK_NOTICELOG("Tests run: %u, failures: %u\n", state.n_tests, state.n_failures);

    execute_spdk_function(exit_io_thread, NULL);
    spdk_thread_send_msg(opts->init_thread, stop_init_thread,
                         state.n_failures ? (void *)0x1 : NULL);
    spdk_thread_exit(spdk_get_thread());
}

static void run_bdev_tests(struct test_state *state, const char *name) {
    int rc = spdk_bdev_open_ext(name, true, ubi_event_cb, NULL, &state->bdev_desc);
    if (rc < 0) {
        SPDK_ERRLOG("Could not open bdev %s: %s\n", name, strerror(-rc));
        return;
    }

    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(state->bdev_desc);
    state->blocklen = bdev->blocklen;
    state->blockcnt = bdev->blockcnt;
    state->n_image_blocks = state->image_size / state->blocklen;

    execute_spdk_function(open_io_channel, state);
    if (state->bdev_desc == NULL || state->ch == NULL) {
        state->n_failures++;
        spdk_bdev_close(state->bdev_desc);
        return;
    }

#define RUN_TEST(x)                                                                      \
    if (!(x)) {                                                                          \
        state->n_failures++;                                                             \
    }

    // read 100 blocks from the image addresses
    RUN_TEST(test_read(state, 10, 100));
    // read 100 blocks from the non-image addresses
    RUN_TEST(test_read(state, state->n_image_blocks + 2, 100));
    // write 100 blocks to the image addresses
    RUN_TEST(test_write(state, 20, 100));
    // write 100 blocks to the non-image addresses
    RUN_TEST(test_write(state, state->n_image_blocks + 2, 100));
    // Some random io
    RUN_TEST(test_random_ops(state, 50));

    execute_spdk_function(close_io_channel, state);
    spdk_bdev_close(state->bdev_desc);
}

static bool test_read(struct test_state *state, uint32_t start, uint32_t count) {
    struct ubi_io_request req;
    state->n_tests++;

    req.state = state;
    for (size_t i = 0; i < count; i++) {
        req.block_idx = start + count;
        execute_spdk_function(ubi_bdev_read, &req);
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

static bool test_write(struct test_state *state, uint32_t start, uint32_t count) {
    struct ubi_io_request read_req, write_req;
    state->n_tests++;

    read_req.state = state;
    write_req.state = state;
    for (size_t i = 0; i < count; i++) {
        write_req.block_idx = start + count;
        read_req.block_idx = start + count;

        for (size_t j = 0; j < state->blocklen; j++) {
            write_req.buf[j] = rand() % 128;
        }

        execute_spdk_function(ubi_bdev_write, &write_req);
        if (!write_req.success) {
            return false;
        }

        execute_spdk_function(ubi_bdev_read, &read_req);
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

static bool test_random_ops(struct test_state *state, uint32_t count) {
    struct ubi_io_request req;
    req.state = state;
    for (size_t i = 0; i < count; i++) {
        int type = rand() % 3;
        req.block_idx = rand() % state->blockcnt;
        switch (type) {
        case 0:
            execute_spdk_function(ubi_bdev_read, &req);
            break;
        case 1:
            execute_spdk_function(ubi_bdev_write, &req);
            break;
        case 2:
            execute_spdk_function(ubi_bdev_flush, &req);
            break;
        }
        if (!req.success)
            return false;
    }

    return true;
}

static bool verify_image_block(struct test_state *state, uint64_t block, char *buf) {
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
