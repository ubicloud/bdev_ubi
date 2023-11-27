#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "bdev_ubi.h"

struct rpc_construct_ubi {
    char *name;
    char *image_path;
    char *base_bdev_name;
    uint32_t stripe_size_mb;
    bool no_sync;
};

static void free_rpc_construct_ubi(struct rpc_construct_ubi *req) {
    free(req->name);
    free(req->image_path);
    free(req->base_bdev_name);
}

static const struct spdk_json_object_decoder rpc_construct_ubi_decoders[] = {
    {"name", offsetof(struct rpc_construct_ubi, name), spdk_json_decode_string},
    {"image_path", offsetof(struct rpc_construct_ubi, image_path),
     spdk_json_decode_string},
    {"base_bdev", offsetof(struct rpc_construct_ubi, base_bdev_name),
     spdk_json_decode_string},
    {"stripe_size_mb", offsetof(struct rpc_construct_ubi, stripe_size_mb),
     spdk_json_decode_uint32, true},
    {"no_sync", offsetof(struct rpc_construct_ubi, no_sync), spdk_json_decode_bool,
     true}};

static void bdev_ubi_create_done(void *cb_arg, struct spdk_bdev *bdev, int status) {
    struct spdk_jsonrpc_request *request = cb_arg;
    if (status < 0) {
        spdk_jsonrpc_send_error_response(request, status, spdk_strerror(-status));
    } else if (status > 0) {
        spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                             "error code: %d.", status);
    } else {
        struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
        spdk_json_write_string(w, bdev->name);
        spdk_jsonrpc_end_result(request, w);
    }
}

/*
 * rpc_bdev_ubi_create handles an rpc request to create a bdev_ubi.
 */
static void rpc_bdev_ubi_create(struct spdk_jsonrpc_request *request,
                                const struct spdk_json_val *params) {
    struct rpc_construct_ubi req = {};
    struct spdk_ubi_bdev_opts opts = {};

    // set optional parameters. spdk_json_decode_object will overwrite if
    // provided.
    req.stripe_size_mb = DEFAULT_STRIPE_SIZE_MB;
    req.no_sync = false;

    if (spdk_json_decode_object(params, rpc_construct_ubi_decoders,
                                SPDK_COUNTOF(rpc_construct_ubi_decoders), &req)) {
        SPDK_DEBUGLOG(bdev_ubi, "spdk_json_decode_object failed\n");
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                         "spdk_json_decode_object failed");
        goto cleanup;
    }

    opts.name = req.name;
    opts.image_path = req.image_path;
    opts.base_bdev_name = req.base_bdev_name;
    opts.stripe_size_mb = req.stripe_size_mb;
    opts.no_sync = req.no_sync;

    struct ubi_create_context *context = calloc(1, sizeof(struct ubi_create_context));
    context->done_fn = bdev_ubi_create_done;
    context->done_arg = request;

    bdev_ubi_create(&opts, context);

cleanup:
    free_rpc_construct_ubi(&req);
}
SPDK_RPC_REGISTER("bdev_ubi_create", rpc_bdev_ubi_create, SPDK_RPC_RUNTIME)

struct rpc_delete_ubi {
    char *name;
};

static void free_rpc_delete_ubi(struct rpc_delete_ubi *req) { free(req->name); }

static const struct spdk_json_object_decoder rpc_delete_ubi_decoders[] = {
    {"name", offsetof(struct rpc_delete_ubi, name), spdk_json_decode_string},
};

static void rpc_bdev_ubi_delete_cb(void *cb_arg, int bdeverrno) {
    struct spdk_jsonrpc_request *request = cb_arg;

    if (bdeverrno == 0) {
        spdk_jsonrpc_send_bool_response(request, true);
    } else {
        spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
    }
}

static void rpc_bdev_ubi_delete(struct spdk_jsonrpc_request *request,
                                const struct spdk_json_val *params) {
    struct rpc_delete_ubi req = {NULL};

    if (spdk_json_decode_object(params, rpc_delete_ubi_decoders,
                                SPDK_COUNTOF(rpc_delete_ubi_decoders), &req)) {
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                         "spdk_json_decode_object failed");
        goto cleanup;
    }

    bdev_ubi_delete(req.name, rpc_bdev_ubi_delete_cb, request);

    free_rpc_delete_ubi(&req);

    return;

cleanup:
    free_rpc_delete_ubi(&req);
}
SPDK_RPC_REGISTER("bdev_ubi_delete", rpc_bdev_ubi_delete, SPDK_RPC_RUNTIME)
