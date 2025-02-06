#include "test_ubi.h"

#define DEFAULT_BDEV_NAME "ubi0"

enum dd_cmdline_opts {
    TEST_OPTION_BDEV = 0x1000,
};

static struct option g_cmdline_opts[] = {{
                                             .name = "bdev",
                                             .has_arg = 1,
                                             .flag = NULL,
                                             .val = TEST_OPTION_BDEV,
                                         },
                                         {.name = NULL}};

static struct test_opts g_opts;

static void test_ubi_run(void *arg1) {
    int i;

    struct spdk_cpuset tmpmask = {};

    if (spdk_env_get_core_count() < 3) {
        spdk_app_stop(-1);
        return;
    }

    g_opts.init_thread = NULL;
    g_opts.io_thread = NULL;
    g_opts.ut_thread = NULL;

    SPDK_ENV_FOREACH_CORE(i) {
        if (i == spdk_env_get_current_core()) {
            g_opts.init_thread = spdk_get_thread();
            continue;
        }
        spdk_cpuset_zero(&tmpmask);
        spdk_cpuset_set_cpu(&tmpmask, i, true);
        if (g_opts.ut_thread == NULL) {
            g_opts.ut_thread = spdk_thread_create("ut_thread", &tmpmask);
        } else if (g_opts.io_thread == NULL) {
            g_opts.io_thread = spdk_thread_create("io_thread", &tmpmask);
        }
    }

    spdk_thread_send_msg(g_opts.ut_thread, run_ut_thread, &g_opts);
}

void stop_init_thread(void *arg) { spdk_app_stop(arg == NULL ? 0 : -1); }

static void bdev_ubi_create_done_cb(void *arg, struct spdk_bdev *bdev, int status) {
    struct ubi_create_request *req = arg;
    req->success = (status == 0);

    wake_ut_thread();
}

void init_thread_create_bdev_ubi(void *arg) {
    struct ubi_create_request *req = arg;
    struct ubi_create_context *context = calloc(1, sizeof(struct ubi_create_context));
    context->done_fn = bdev_ubi_create_done_cb;
    context->done_arg = req;

    bdev_ubi_create(&req->opts, context);
}

static void bdev_ubi_delete_done_cb(void *arg, int status) {
    struct ubi_delete_request *req = arg;
    req->success = (status == 0);

    wake_ut_thread();
}

void init_thread_delete_bdev_ubi(void *arg) {
    struct ubi_delete_request *req = arg;
    bdev_ubi_delete(req->name, bdev_ubi_delete_done_cb, req);
}

static void usage(void) { printf("  -bdev Block device to be used for testing.\n"); }

static int parse_arg(int opt, char *arg) {
    if (opt == TEST_OPTION_BDEV) {
        if (g_opts.n_bdevs >= MAX_BDEVS) {
            fprintf(stderr, "Too many bdevs.\n");
            exit(-1);
        }
        g_opts.bdev_names[g_opts.n_bdevs++] = strdup(arg);
    }
    return 0;
}

int main(int argc, char **argv) {
    int rc;
    struct spdk_app_opts opts = {};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "test_ubi";

    memset(&g_opts, 0, sizeof(g_opts));

    rc = spdk_app_parse_args(argc, argv, &opts, NULL, g_cmdline_opts, parse_arg, usage);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        exit(rc);
    }

    if (g_opts.n_bdevs == 0) {
        g_opts.n_bdevs = 1;
        g_opts.bdev_names[0] = strdup(DEFAULT_BDEV_NAME);
    }

    rc = spdk_app_start(&opts, test_ubi_run, NULL);
    if (rc) {
        SPDK_ERRLOG("Error occurred while testing bdev_ubi.\n");
    }

    for (int i = 0; i < g_opts.n_bdevs; i++)
        free(g_opts.bdev_names[i]);

    spdk_app_fini();

    return rc;
}
