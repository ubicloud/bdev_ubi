#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/event.h"
#include "spdk/fd.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vmd.h"
#include <stdio.h>

#define BDEV_NAME "ubi0"
#define IMAGE_PATH "build/bin/test_image.raw"
#define MAX_BLOCK_SIZE 4096
#define IMAGE_SIZE (40 * 1024 * 1024)

struct test_state {
    struct spdk_bdev_desc *bdev_desc;
    struct spdk_io_channel *ch;
    FILE *image_file;

    uint32_t blocklen;
    uint64_t n_image_blocks;

    uint32_t n_tests;
    uint32_t n_failures;
};

struct ubi_io_request {
    char buf[MAX_BLOCK_SIZE];
    uint64_t block_idx;
    struct test_state *state;

    bool success;
};

/* forward declarations */
static void test_ubi_run(void *arg1);

static void open_bdev(void *arg);
static void close_bdev(void *arg);
static void exit_io_thread(void *arg);
static void ubi_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
                         void *event_ctx);
static void ubi_bdev_write(void *arg);
static void ubi_bdev_read(void *arg);
static void io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg);

static void run_ut_thread(void *arg);
static bool test_read(struct test_state *state, uint32_t start, uint32_t count);
static bool test_write(struct test_state *state, uint32_t start, uint32_t count);
static bool verify_image_block(struct test_state *state, uint64_t block, char *buf);
static bool file_size(FILE *f, uint64_t *out);

struct spdk_thread *g_init_thread;
struct spdk_thread *g_io_thread;
struct spdk_thread *g_ut_thread;
pthread_mutex_t g_test_mutex;
pthread_cond_t g_test_cond;
struct test_state g_state;

static void execute_spdk_function(spdk_msg_fn fn, void *arg) {
    pthread_mutex_lock(&g_test_mutex);
    spdk_thread_send_msg(g_io_thread, fn, arg);
    pthread_cond_wait(&g_test_cond, &g_test_mutex);
    pthread_mutex_unlock(&g_test_mutex);
}

static void wake_ut_thread(void) {
    pthread_mutex_lock(&g_test_mutex);
    pthread_cond_signal(&g_test_cond);
    pthread_mutex_unlock(&g_test_mutex);
}

/*
 * main thread functions
 */

int main(int argc, char **argv) {
    int rc;
    struct spdk_app_opts opts = {};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "test_ubi";
    opts.reactor_mask = "0x1";

    rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        exit(rc);
    }

    rc = spdk_app_start(&opts, test_ubi_run, NULL);
    if (rc) {
        SPDK_ERRLOG("Error occured while testing bdev_ubi.\n");
    }

    spdk_app_fini();

    return rc;
}

static void test_ubi_run(void *arg1) {
    int i;

    pthread_mutex_init(&g_test_mutex, NULL);
    pthread_cond_init(&g_test_cond, NULL);

    struct spdk_cpuset tmpmask = {};

    if (spdk_env_get_core_count() < 3) {
        spdk_app_stop(-1);
        return;
    }

    SPDK_ENV_FOREACH_CORE(i) {
        if (i == spdk_env_get_current_core()) {
            g_init_thread = spdk_get_thread();
            continue;
        }
        spdk_cpuset_zero(&tmpmask);
        spdk_cpuset_set_cpu(&tmpmask, i, true);
        if (g_ut_thread == NULL) {
            g_ut_thread = spdk_thread_create("ut_thread", &tmpmask);
        } else if (g_io_thread == NULL) {
            g_io_thread = spdk_thread_create("io_thread", &tmpmask);
        }
    }

    spdk_thread_send_msg(g_ut_thread, run_ut_thread, NULL);
}

static void stop_init_thread(void *arg) {
    struct test_state *state = arg;

    SPDK_NOTICELOG("Tests run: %u, failures: %u\n", state->n_tests, state->n_failures);

    execute_spdk_function(exit_io_thread, NULL);
    spdk_app_stop(state->n_failures);
}

/*
 * IO thread functions
 */

static void exit_io_thread(void *arg) {
    spdk_thread_exit(g_io_thread);
    wake_ut_thread();
}

static void open_bdev(void *arg) {
    struct test_state *state = arg;
    int rc;
    rc = spdk_bdev_open_ext(BDEV_NAME, true, ubi_event_cb, NULL, &state->bdev_desc);
    if (rc < 0) {
        SPDK_ERRLOG("Could not open bdev %s: %s\n", BDEV_NAME, strerror(-rc));
        goto done;
    }

    state->ch = spdk_bdev_get_io_channel(state->bdev_desc);
    if (state->ch == NULL) {
        SPDK_ERRLOG("Could not get I/O channel: %s\n", strerror(ENOMEM));
        goto done;
    }

    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(state->bdev_desc);
    state->blocklen = bdev->blocklen;

done:
    wake_ut_thread();
}

static void close_bdev(void *arg) {
    struct test_state *state = arg;
    if (state->ch)
        spdk_put_io_channel(state->ch);
    if (state->bdev_desc)
        spdk_bdev_close(state->bdev_desc);

    wake_ut_thread();
}

static void ubi_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
                         void *event_ctx) {
    SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}

static void ubi_bdev_write(void *arg) {
    struct ubi_io_request *req = arg;

    int rc = spdk_bdev_write_blocks(req->state->bdev_desc, req->state->ch, req->buf,
                                    req->block_idx, 1, io_completion_cb, req);

    if (rc) {
        wake_ut_thread();
    }
}

static void ubi_bdev_read(void *arg) {
    struct ubi_io_request *req = arg;

    // Reset success. This will be set in the completion callback.
    req->success = false;

    int rc = spdk_bdev_read_blocks(req->state->bdev_desc, req->state->ch, req->buf,
                                   req->block_idx, 1, io_completion_cb, req);

    if (rc) {
        wake_ut_thread();
    }
}

static void io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg) {
    struct ubi_io_request *req = arg;
    req->success = success;
    spdk_bdev_free_io(bdev_io);
    wake_ut_thread();
}

/*
 * UT thread functions
 */

static void run_ut_thread(void *arg) {
    int rc = 0;
    memset(&g_state, 0, sizeof(g_state));
    struct test_state *state = &g_state;

    execute_spdk_function(open_bdev, state);
    if (state->bdev_desc == NULL || state->ch == NULL) {
        state->n_failures++;
        goto done;
    }

    state->image_file = fopen(IMAGE_PATH, "r");
    if (state->image_file == NULL) {
        rc = errno;
        SPDK_ERRLOG("Could not open %s: %s\n", IMAGE_PATH, strerror(-rc));
        state->n_failures++;
        goto done;
    }

    uint64_t image_size = 0;
    if (!file_size(state->image_file, &image_size)) {
        state->n_failures++;
        goto done;
    }

    state->n_image_blocks = image_size / state->blocklen;
    SPDK_NOTICELOG("Image block count: %lu.\n", state->n_image_blocks);

#define RUN_TEST(x)                                                                      \
    if (!(x)) {                                                                          \
        state->n_failures++;                                                             \
    }

    // read 100 blocks from the image addresses
    RUN_TEST(test_read(state, 10, 100));
    // read 100 blocks from the non-image addresses
    RUN_TEST(test_read(state, 81922, 100));
    // write 100 blocks to the image addresses
    RUN_TEST(test_write(state, 20, 100));
    // write 100 blocks to the non-image addresses
    RUN_TEST(test_write(state, 82922, 100));

    execute_spdk_function(close_bdev, state);

done:
    if (state->image_file)
        fclose(state->image_file);

    spdk_thread_send_msg(g_init_thread, stop_init_thread, state);
    spdk_thread_exit(g_ut_thread);
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
