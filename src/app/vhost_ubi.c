#include "spdk/event.h"
#include "spdk/stdinc.h"
#include "spdk/vhost.h"

static void vhost_usage(void) {
    printf(" -S <path>     directory where to create vhost sockets (default: "
           "pwd)\n");
}

static int vhost_parse_arg(int ch, char *arg) {
    switch (ch) {
    case 'S':
        spdk_vhost_set_socket_path(arg);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static void vhost_started(void *arg1) {}

int main(int argc, char *argv[]) {
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "vhost_ubi";

    rc = spdk_app_parse_args(argc, argv, &opts, "f:S:", NULL, vhost_parse_arg,
                             vhost_usage);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        exit(rc);
    }

    /* Blocks until the application is exiting */
    rc = spdk_app_start(&opts, vhost_started, NULL);

    spdk_app_fini();

    return rc;
}
