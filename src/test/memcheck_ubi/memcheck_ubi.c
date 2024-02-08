#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/event.h"
#include "spdk/fd.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vmd.h"
#include <stdio.h>
#include <string.h>

#define DEFAULT_BDEV_NAME "ubi0"

#define BLOCK_SIZE 512
#define MAX_OPS 5000
#define MAX_BDEVS 10

struct {
    char *bdev_names[MAX_BDEVS];
    int n_bdevs;
} g_opts;

size_t g_bdevs_tested = 0;
struct test_state {
    struct spdk_bdev_desc *bdev_desc;
    struct spdk_io_channel *ch;

    int n_ops_queued;
    int n_ops_finished;
    int n_ops_target;

    char buf[BLOCK_SIZE];
} g_test_state;

enum dd_cmdline_opts {
    MEMCHECK_OPTION_BDEV = 0x1000,
};

static struct option g_cmdline_opts[] = {{
                                             .name = "bdev",
                                             .has_arg = 1,
                                             .flag = NULL,
                                             .val = MEMCHECK_OPTION_BDEV,
                                         },
                                         {.name = NULL}};

static void enqueue_io_ops(void *arg);
static void open_bdev(void *arg);

#define continue_with_fn(fn)                                                             \
    {                                                                                    \
        spdk_thread_send_msg(spdk_get_thread(), fn, NULL);                               \
        return;                                                                          \
    }

static void ubi_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
                         void *event_ctx) {
    SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}

static void close_bdev(void) {
    if (g_test_state.ch)
        spdk_put_io_channel(g_test_state.ch);
    if (g_test_state.bdev_desc)
        spdk_bdev_close(g_test_state.bdev_desc);
}

static void fail_tests(void *arg) {
    close_bdev();

    SPDK_ERRLOG("Tests failed.\n");
    spdk_app_stop(-1);
}

static void succeed_tests(void *arg) {
    SPDK_NOTICELOG("Tests succeeded.\n");
    spdk_app_stop(0);
}

static void io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg) {
    spdk_bdev_free_io(bdev_io);
    if (!success)
        continue_with_fn(fail_tests);
    g_test_state.n_ops_finished++;

    // there's still some io to be finished
    if (g_test_state.n_ops_finished != g_test_state.n_ops_queued)
        return;

    // reached target op count. continue with testing the next bdev.
    if (g_test_state.n_ops_finished >= g_test_state.n_ops_target) {
        close_bdev();
        continue_with_fn(open_bdev);
    }

    continue_with_fn(enqueue_io_ops);
}

static bool enqueue_write(size_t offset) {
    int rc = spdk_bdev_write_blocks(g_test_state.bdev_desc, g_test_state.ch,
                                    g_test_state.buf, offset, 1, io_completion_cb, NULL);
    return rc == 0;
}

static bool enqueue_read(size_t offset) {
    int rc = spdk_bdev_read_blocks(g_test_state.bdev_desc, g_test_state.ch,
                                   g_test_state.buf, offset, 1, io_completion_cb, NULL);
    return rc == 0;
}

static bool enqueue_flush(size_t offset) {
    int rc = spdk_bdev_flush_blocks(g_test_state.bdev_desc, g_test_state.ch, offset, 1,
                                    io_completion_cb, NULL);
    return rc == 0;
}

static void enqueue_io_ops(void *arg) {
    int remaining_ops = g_test_state.n_ops_target - g_test_state.n_ops_queued;

    int count = rand() % 10 + 1;
    if (count > remaining_ops)
        count = remaining_ops;

    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(g_test_state.bdev_desc);
    size_t blockcnt = bdev->blockcnt;

    for (int i = 0; i < count; i++) {
        int type = rand() % 3;
        size_t offset = rand() % blockcnt;
        bool success = false;
        switch (type) {
        case 0:
            success = enqueue_write(offset);
            break;
        case 1:
            success = enqueue_read(offset);
            break;
        case 2:
            success = enqueue_flush(offset);
            break;
        }
        if (!success)
            continue_with_fn(fail_tests);
    }

    g_test_state.n_ops_queued += count;
}

static void open_bdev(void *arg) {
    if (g_bdevs_tested >= g_opts.n_bdevs) {
        continue_with_fn(succeed_tests);
    }

    char *name = g_opts.bdev_names[g_bdevs_tested++];
    memset(&g_test_state, 0, sizeof(g_test_state));
    g_test_state.n_ops_target = MAX_OPS;

    int rc = spdk_bdev_open_ext(name, true, ubi_event_cb, NULL, &g_test_state.bdev_desc);
    if (rc < 0) {
        SPDK_ERRLOG("Could not open bdev %s: %s\n", name, strerror(-rc));
        continue_with_fn(fail_tests);
    }

    g_test_state.ch = spdk_bdev_get_io_channel(g_test_state.bdev_desc);
    if (g_test_state.ch == NULL) {
        SPDK_ERRLOG("Could not get I/O channel: %s\n", strerror(ENOMEM));
        continue_with_fn(fail_tests);
    }

    continue_with_fn(enqueue_io_ops);
};

static void start_tests(void *arg) { continue_with_fn(open_bdev); }

static void usage(void) { printf("  -bdev Block device to be used for testing.\n"); }

static int parse_arg(int argc, char *argv) {
    switch (argc) {
    case MEMCHECK_OPTION_BDEV:
        if (g_opts.n_bdevs >= MAX_BDEVS) {
            fprintf(stderr, "Too many bdevs.\n");
            exit(-1);
        }
        g_opts.bdev_names[g_opts.n_bdevs++] = strdup(argv);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

int main(int argc, char **argv) {
    int rc;
    struct spdk_app_opts opts = {};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "test_ubi";
    opts.reactor_mask = "0x1";

    rc = spdk_app_parse_args(argc, argv, &opts, NULL, g_cmdline_opts, parse_arg, usage);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        exit(rc);
    }

    if (g_opts.n_bdevs == 0) {
        g_opts.n_bdevs = 1;
        g_opts.bdev_names[0] = strdup(DEFAULT_BDEV_NAME);
    }

    rc = spdk_app_start(&opts, start_tests, NULL);
    if (rc) {
        SPDK_ERRLOG("Error occured while testing bdev_ubi.\n");
    }

    for (int i = 0; i < g_opts.n_bdevs; i++)
        free(g_opts.bdev_names[i]);

    spdk_app_fini();

    return rc;
}
