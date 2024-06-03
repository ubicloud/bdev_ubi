#include "bdev_ubi.h"
#include "bdev_ubi_internal.h"

#include "spdk/fd.h"
#include "spdk/likely.h"
#include "spdk/log.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

/*
 * Static function forward declarations
 */
static int ubi_initialize(void);
static void ubi_finish(void);
static int ubi_get_ctx_size(void);
static int ubi_destruct(void *ctx);
static void ubi_close_base_bdev(void *ctx);
static int ubi_init_layout_params(struct ubi_bdev *ubi_bdev);
static void ubi_start_read_metadata(struct ubi_bdev *ubi_bdev,
                                    struct ubi_create_context *context);
static void ubi_finish_read_metadata(struct spdk_bdev_io *bdev_io, bool success,
                                     void *cb_arg);
static bool ubi_new_disk(const uint8_t *magic);
static void ubi_init_metadata(struct ubi_bdev *ubi_bdev);
static void ubi_finish_create(int status, struct ubi_create_context *context);
static void ubi_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static bool ubi_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);
static struct spdk_io_channel *ubi_get_io_channel(void *ctx);
static void ubi_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);
static void ubi_handle_base_bdev_event(enum spdk_bdev_event_type type,
                                       struct spdk_bdev *bdev, void *event_ctx);
static bool ubi_bdev_find_by_base_bdev(struct spdk_bdev *base_bdev,
                                       struct ubi_bdev **_ubi_bdev,
                                       struct ubi_base_bdev_info **_base_info);
static void ubi_handle_base_bdev_remove_event(struct spdk_bdev *base_bdev);
static void ubi_set_version(struct ubi_metadata *metadata, uint16_t major,
                            uint16_t minor);
static void ubi_get_version(struct ubi_metadata *metadata, uint16_t *major,
                            uint16_t *minor);

/*
 * Module Interface
 */
static struct spdk_bdev_module ubi_if = {
    .name = "ubi",
    .module_init = ubi_initialize,
    .module_fini = ubi_finish,
    .async_fini = false,
    .async_init = false,
    .get_ctx_size = ubi_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(ubi, &ubi_if)

/*
 * Block device operations
 */
static const struct spdk_bdev_fn_table ubi_fn_table = {
    .destruct = ubi_destruct,
    .submit_request = ubi_submit_request,
    .io_type_supported = ubi_io_type_supported,
    .get_io_channel = ubi_get_io_channel,
    .write_config_json = ubi_write_config_json,
};

static TAILQ_HEAD(, ubi_bdev) g_ubi_bdev_head = TAILQ_HEAD_INITIALIZER(g_ubi_bdev_head);

/*
 * ubi_initialize is called when the module is initialized.
 */
static int ubi_initialize(void) { return 0; }

/*
 * ubi_finish is called when the module is finished.
 */
static void ubi_finish(void) {}

/*
 * ubi_get_ctx_size returns the size of I/O cotnext.
 */
static int ubi_get_ctx_size(void) { return sizeof(struct ubi_bdev_io); }

/*
 * bdev_ubi_create. Creates a ubi_bdev and registers it.
 *
 * This will always call the callback stored in context->done_fn with the
 * success status.
 */
void bdev_ubi_create(const struct spdk_ubi_bdev_opts *opts,
                     struct ubi_create_context *context) {
    struct ubi_bdev *ubi_bdev;
    int rc;

    if (!opts) {
        SPDK_ERRLOG("No options provided for Ubi bdev %s.\n", opts->name);
        ubi_finish_create(-EINVAL, context);
        return;
    }

    /*
     * By using calloc() we initialize the memory region to all 0, which also
     * ensures that metadata, strip_status, and metadata_dirty are all 0
     * initially.
     */
    ubi_bdev = calloc(1, sizeof(struct ubi_bdev));
    if (!ubi_bdev) {
        SPDK_ERRLOG("could not allocate ubi_bdev %s\n", opts->name);
        ubi_finish_create(-ENOMEM, context);
        return;
    }

    /*
     * Save ubi_bdev in context so we can either register it (on success) or
     * clean it up (on failure) in ubi_finish_create().
     */
    context->ubi_bdev = ubi_bdev;

    ubi_bdev->bdev.name = strdup(opts->name);
    if (!ubi_bdev->bdev.name) {
        SPDK_ERRLOG("could not duplicate name for ubi_bdev %s\n", opts->name);
        ubi_finish_create(-ENOMEM, context);
        return;
    }

    /* Save the thread where the base device is opened. */
    ubi_bdev->thread = spdk_get_thread();

    rc = configure_base_bdev(opts->base_bdev_name, true, &ubi_bdev->base_bdev_info);
    if (rc) {
        UBI_ERRLOG(ubi_bdev, "could not get base image bdev\n");
        ubi_finish_create(rc, context);
        return;
    }

    /*
     * Initialize variables that determine the layout of both metadata and
     * actual data on base bdev.
     *
     * TODO: if this is not a new disk, we should read stripe_size_kb from
     * metadata.
     */
    ubi_bdev->stripe_size_kb = opts->stripe_size_kb;
    ubi_bdev->no_sync = opts->no_sync;

    ubi_bdev->copy_on_read = opts->copy_on_read;
    ubi_bdev->directio = opts->directio;

    strncpy(ubi_bdev->image_path, opts->image_path, UBI_PATH_LEN);
    ubi_bdev->image_path[UBI_PATH_LEN - 1] = 0;

    rc = ubi_init_layout_params(ubi_bdev);
    if (rc) {
        ubi_finish_create(rc, context);
        return;
    }

    /* Copy some properties from the underlying base bdev. */
    struct spdk_bdev *base_bdev = ubi_bdev->base_bdev_info.bdev;
    ubi_bdev->bdev.blocklen = base_bdev->blocklen;
    ubi_bdev->bdev.blockcnt = base_bdev->blockcnt - ubi_bdev->data_offset_blocks;
    ubi_bdev->bdev.write_cache = base_bdev->write_cache;

    ubi_bdev->bdev.product_name = "Ubi disk";
    ubi_bdev->bdev.ctxt = ubi_bdev;
    ubi_bdev->bdev.fn_table = &ubi_fn_table;
    ubi_bdev->bdev.module = &ubi_if;

    ubi_bdev->bdev.optimal_io_boundary = ubi_bdev->stripe_block_count;
    ubi_bdev->bdev.split_on_optimal_io_boundary = true;

    ubi_bdev->alignment_bytes = 4096;
    ubi_bdev->bdev.required_alignment = spdk_u32log2(ubi_bdev->alignment_bytes);

    spdk_io_device_register(ubi_bdev, ubi_create_channel_cb, ubi_destroy_channel_cb,
                            sizeof(struct ubi_io_channel), ubi_bdev->bdev.name);
    context->registerd = true;

    // read metadata asynchronously.
    ubi_start_read_metadata(ubi_bdev, context);
}

/*
 * ubi_start_read_metadata initiates reading metadata from base bdev and
 * returns. This doesn't block. Instead ubi_finish_read_metadata is called when
 * reading is done.
 */
static void ubi_start_read_metadata(struct ubi_bdev *ubi_bdev,
                                    struct ubi_create_context *context) {
    struct spdk_bdev_desc *base_desc = ubi_bdev->base_bdev_info.desc;
    context->base_ch = spdk_bdev_get_io_channel(base_desc);
    int offset = 0;
    int block_cnt = UBI_METADATA_SIZE / ubi_bdev->bdev.blocklen;
    int ret = spdk_bdev_read_blocks(base_desc, context->base_ch, &ubi_bdev->metadata,
                                    offset, block_cnt, ubi_finish_read_metadata, context);
    if (ret) {
        ubi_finish_create(ret, context);
    }
}

/*
 * ubi_finish_read_metadata is called when reading metadata from base bdev
 * finishes.
 */
static void ubi_finish_read_metadata(struct spdk_bdev_io *bdev_io, bool success,
                                     void *cb_arg) {
    struct ubi_create_context *context = cb_arg;
    spdk_bdev_free_io(bdev_io);
    spdk_put_io_channel(context->base_ch);

    if (!success) {
        ubi_finish_create(-EIO, context);
        return;
    }

    struct ubi_metadata *metadata = &context->ubi_bdev->metadata;
    if (ubi_new_disk(metadata->magic)) {
        ubi_init_metadata(context->ubi_bdev);
        ubi_finish_create(0, context);
        return;
    } else if (memcmp(UBI_MAGIC, metadata->magic, UBI_MAGIC_SIZE)) {
        UBI_ERRLOG(context->ubi_bdev, "Invalid magic.\n");
        ubi_finish_create(-EINVAL, context);
        return;
    }

    uint16_t versionMajor, versionMinor;
    ubi_get_version(metadata, &versionMajor, &versionMinor);
    if (versionMajor != UBI_VERSION_MAJOR || versionMinor != UBI_VERSION_MINOR) {
        UBI_ERRLOG(context->ubi_bdev, "Unsupported metadata version: %d.%d", versionMajor,
                   versionMinor);
        ubi_finish_create(-EINVAL, context);
        return;
    }

    for (int i = 0; i < UBI_MAX_STRIPES; i++) {
        bool fetched = metadata->stripe_headers[i][0];
        context->ubi_bdev->stripe_status[i] =
            fetched ? STRIPE_FETCHED : STRIPE_NOT_FETCHED;
        if (fetched) {
            context->ubi_bdev->stripes_fetched++;
            context->ubi_bdev->stripes_flushed++;
        }
    }

    ubi_finish_create(0, context);
}

static bool ubi_new_disk(const uint8_t *magic) {
    /*
     * Assume a new disk if metadata has been zeroed out.
     */
    for (size_t i = 0; i < UBI_MAGIC_SIZE; i++)
        if (magic[i] != 0)
            return false;
    return true;
}

static void ubi_init_metadata(struct ubi_bdev *ubi_bdev) {
    memcpy(ubi_bdev->metadata.magic, UBI_MAGIC, UBI_MAGIC_SIZE);
    ubi_set_version(&ubi_bdev->metadata, UBI_VERSION_MAJOR, UBI_VERSION_MINOR);
    ubi_bdev->metadata.stripe_size_kb = ubi_bdev->stripe_size_kb;
}

/*
 * ubi_finish_create is called when the ubi_bdev creation flow is done, either
 * by success or failure. Non-zero "status" means a failure.
 */
static void ubi_finish_create(int status, struct ubi_create_context *context) {
    struct ubi_bdev *ubi_bdev = context->ubi_bdev;

    if (status == 0) {
        status = spdk_bdev_register(&ubi_bdev->bdev);
        if (status != 0) {
            UBI_ERRLOG(ubi_bdev, "could not register ubi_bdev\n");
            spdk_bdev_module_release_bdev(&ubi_bdev->bdev);
        } else {
            TAILQ_INSERT_TAIL(&g_ubi_bdev_head, ubi_bdev, tailq);
        }
    }

    if (status != 0 && ubi_bdev) {
        if (ubi_bdev->base_bdev_info.desc) {
            spdk_bdev_close(ubi_bdev->base_bdev_info.desc);
        }

        if (context->registerd) {
            spdk_io_device_unregister(ubi_bdev, NULL);
        }

        free(ubi_bdev->bdev.name);
        free(ubi_bdev);
    }

    context->done_fn(context->done_arg, &ubi_bdev->bdev, status);
    free(context);
}

/*
 * ubi_init_layout_params initializes variables related to layout of metadata
 * and block data are layed out on base bdev.
 */
static int ubi_init_layout_params(struct ubi_bdev *ubi_bdev) {
    // ensure base image exists, and get its size
    struct stat statBuffer;
    int statResult = stat(ubi_bdev->image_path, &statBuffer);
    if (statResult < 0) {
        UBI_ERRLOG(ubi_bdev, "getting stats for %s failed: %s\n", ubi_bdev->image_path,
                   strerror(errno));
        return -EINVAL;
    }

    uint32_t blocklen = ubi_bdev->base_bdev_info.bdev->blocklen;
    uint64_t blockcnt = ubi_bdev->base_bdev_info.bdev->blockcnt;

    if (blockcnt * blocklen < (uint64_t)statBuffer.st_size + UBI_METADATA_SIZE) {
        UBI_ERRLOG(ubi_bdev, "base block device is smaller than image + metadata size\n");
        return -EINVAL;
    }

    if (ubi_bdev->stripe_size_kb < UBI_STRIPE_SIZE_MIN ||
        ubi_bdev->stripe_size_kb > UBI_STRIPE_SIZE_MAX) {
        UBI_ERRLOG(ubi_bdev, "stripe_size_kb must be between %d and %d (inclusive)\n",
                   UBI_STRIPE_SIZE_MIN, UBI_STRIPE_SIZE_MAX);
        return -EINVAL;
    }

    if (ubi_bdev->stripe_size_kb & (ubi_bdev->stripe_size_kb - 1)) {
        UBI_ERRLOG(ubi_bdev, "stripe_size_kb must be a power of 2\n");
        return -EINVAL;
    }

    uint32_t stripSizeBytes = ubi_bdev->stripe_size_kb * 1024;
    if (stripSizeBytes < blocklen) {
        UBI_ERRLOG(ubi_bdev,
                   "stripe size (%u bytes) can't be less than base bdev's "
                   "blocklen (%u bytes)",
                   stripSizeBytes, blocklen);
        return -EINVAL;
    }

    if (UBI_METADATA_SIZE % blocklen) {
        UBI_ERRLOG(ubi_bdev, "metadata size (%d) must be a multiple of blocklen (%d)\n",
                   UBI_METADATA_SIZE, blocklen);
        return -EINVAL;
    }

    uint32_t r = (stripSizeBytes + blocklen - 1) / blocklen;
    int log2_r = 0;
    while (r > 1) {
        r /= 2;
        log2_r++;
    }

    ubi_bdev->stripe_block_count = (1 << log2_r);
    ubi_bdev->stripe_shift = log2_r;
    ubi_bdev->data_offset_blocks = UBI_METADATA_SIZE / blocklen;
    ubi_bdev->image_block_count = (statBuffer.st_size + blocklen - 1) / blocklen;

    return 0;
}

/*
 * bdev_ubi_delete. Finds and unregisters a given bdev name.
 */
void bdev_ubi_delete(const char *bdev_name, spdk_delete_ubi_complete cb_fn,
                     void *cb_arg) {
    int rc;
    rc = spdk_bdev_unregister_by_name(bdev_name, &ubi_if, cb_fn, cb_arg);
    if (rc != 0) {
        cb_fn(cb_arg, rc);
    }
}

/* Callback for unregistering the IO device. */
static void _device_unregister_cb(void *io_device) {
    struct ubi_bdev *ubi_bdev = io_device;

    /* Done with this ubi_bdev. */
    free(ubi_bdev->bdev.name);
    free(ubi_bdev);
}

/*
 * ubi_destruct. Given a pointer to a ubi_bdev, destruct it.
 */
static int ubi_destruct(void *ctx) {
    struct ubi_bdev *ubi_bdev = ctx;

    TAILQ_REMOVE(&g_ubi_bdev_head, ubi_bdev, tailq);

    /* Unclaim the underlying bdev. */
    spdk_bdev_module_release_bdev(ubi_bdev->base_bdev_info.bdev);

    /* Close the underlying bdev in the same thread it was opened. */
    if (ubi_bdev->thread && ubi_bdev->thread != spdk_get_thread()) {
        spdk_thread_send_msg(ubi_bdev->thread, ubi_close_base_bdev,
                             ubi_bdev->base_bdev_info.desc);
    } else {
        spdk_bdev_close(ubi_bdev->base_bdev_info.desc);
    }

    /* Unregister the io_device. */
    spdk_io_device_unregister(ubi_bdev, _device_unregister_cb);

    return 0;
}

/*
 * ubi_close_base_bdev closes bdev given as context.
 */
static void ubi_close_base_bdev(void *ctx) {
    struct spdk_bdev_desc *desc = ctx;

    spdk_bdev_close(desc);
}

/*
 * ubi_write_config_json writes out config parameters for the given bdev to a
 * json writer.
 */
static void ubi_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w) {
    struct ubi_bdev *ubi_bdev = bdev->ctxt;

    spdk_json_write_object_begin(w);

    spdk_json_write_named_string(w, "method", "bdev_ubi_create");

    spdk_json_write_named_object_begin(w, "params");
    spdk_json_write_named_string(w, "name", bdev->name);
    spdk_json_write_named_string(w, "base_bdev", ubi_bdev->base_bdev_info.bdev->name);
    spdk_json_write_named_string(w, "image_path", ubi_bdev->image_path);
    spdk_json_write_named_uint32(w, "stripe_size_kb", ubi_bdev->stripe_size_kb);
    spdk_json_write_object_end(w);

    spdk_json_write_object_end(w);
}

/*
 * ubi_io_type_supported determines which I/O operations are supported.
 */
static bool ubi_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type) {
    /*
     * According to https://spdk.io/doc/bdev_module.html, only READ and WRITE
     * are necessary. We also support FLUSH to provide crash recovery.
     */
    switch (io_type) {
    case SPDK_BDEV_IO_TYPE_READ:
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_FLUSH:
        return true;
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
        /*
         * Write zeros to given address range. We don't support it explicitly
         * yet. Generic bdev code is capable if emulating this by sending
         * regular writes.
         */
    case SPDK_BDEV_IO_TYPE_RESET:
        /*
         * Request to abort all I/O and return the underlying device to its
         * initial state.
         *
         * Not supported yet.
         */
    case SPDK_BDEV_IO_TYPE_UNMAP:
        /*
         * Often referred to as "trim" or "deallocate", and is a request to
         * mark a set of blocks as no longer containing valid data.
         *
         * Not supported yet.
         */
    default:
        return false;
    }
}

/*
 * ubi_submit_request is called when an I/O request arrives. It will enqueue
 * an stripe fetch if necessary, and then enqueue the I/O request so it is
 * served in the poller.
 */
static void ubi_submit_request(struct spdk_io_channel *_ch,
                               struct spdk_bdev_io *bdev_io) {
    struct ubi_io_channel *ch = spdk_io_channel_get_ctx(_ch);
    struct ubi_bdev *ubi_bdev = bdev_io->bdev->ctxt;

    if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE ||
        (bdev_io->type == SPDK_BDEV_IO_TYPE_READ && ubi_bdev->copy_on_read)) {

        uint64_t start_block = bdev_io->u.bdev.offset_blocks;
        uint64_t num_blocks = bdev_io->u.bdev.num_blocks;
        uint64_t end_block = start_block + num_blocks - 1;

        uint64_t start_stripe = start_block >> ubi_bdev->stripe_shift;
        uint64_t end_stripe = end_block >> ubi_bdev->stripe_shift;
        if (start_stripe != end_stripe) {
            /*
             * this shouldn't happen because we set split_on_optimal_io_boundary
             * to true.
             */
            UBI_ERRLOG(ubi_bdev, "BUG: I/O cannot span stripe boundary!\n");
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            return;
        }

        if (start_block < ubi_bdev->image_block_count &&
            ubi_get_stripe_status(ubi_bdev, start_stripe) == STRIPE_NOT_FETCHED) {
            enqueue_stripe(ch, start_stripe);
            ubi_set_stripe_status(ubi_bdev, start_stripe, STRIPE_INFLIGHT);
        }
    }

    TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
}

/*
 * ubi_get_io_channel return I/O channel pointer for the given ubi bdev.
 */
static struct spdk_io_channel *ubi_get_io_channel(void *ctx) {
    struct bdev_ubi *bdev_ubi = ctx;

    return spdk_get_io_channel(bdev_ubi);
}

/*
 * configure_base_bdev opens and claims a bdev identified by then given name.
 * Fills in "base_info" and returns success status.
 */
int configure_base_bdev(const char *name, bool write,
                        struct ubi_base_bdev_info *base_info) {
    struct spdk_bdev_desc *desc;
    struct spdk_bdev *bdev;
    int rc;

    assert(spdk_get_thread() == spdk_thread_get_app_thread());

    rc = spdk_bdev_open_ext(name, write, ubi_handle_base_bdev_event, NULL, &desc);
    if (rc != 0) {
        if (rc != -ENODEV) {
            SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", name);
        }
        return rc;
    }

    bdev = spdk_bdev_desc_get_bdev(desc);

    rc = spdk_bdev_module_claim_bdev(bdev, desc, &ubi_if);
    if (rc != 0) {
        SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
        spdk_bdev_close(desc);
        return rc;
    }

    base_info->bdev = bdev;
    base_info->desc = desc;

    return 0;
}

/*
 * ubi_handle_base_bdev_remove_event is callback which is called when of base
 * bdevs trigger an event, e.g. when they're removed or resized.
 */
static void ubi_handle_base_bdev_event(enum spdk_bdev_event_type type,
                                       struct spdk_bdev *bdev, void *event_ctx) {
    switch (type) {
    case SPDK_BDEV_EVENT_REMOVE:
        ubi_handle_base_bdev_remove_event(bdev);
        break;
    default:
        SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
        break;
    }
}

/*
 * ubi_handle_base_bdev_remove_event is called if a base bdev is removed.
 */
static void ubi_handle_base_bdev_remove_event(struct spdk_bdev *base_bdev) {
    struct ubi_bdev *ubi_bdev = NULL;
    struct ubi_base_bdev_info *base_info;

    if (!ubi_bdev_find_by_base_bdev(base_bdev, &ubi_bdev, &base_info)) {
        SPDK_ERRLOG("bdev to remove '%s' not found\n", base_bdev->name);
        return;
    }

    spdk_bdev_module_release_bdev(base_bdev);
    spdk_bdev_close(base_info->desc);
}

/*
 * ubi_bdev_find_by_base_bdev finds the bdev which owns the given base bdev.
 */
static bool ubi_bdev_find_by_base_bdev(struct spdk_bdev *base_bdev,
                                       struct ubi_bdev **_ubi_bdev,
                                       struct ubi_base_bdev_info **_base_info) {
    struct ubi_bdev *ubi_bdev;

    TAILQ_FOREACH(ubi_bdev, &g_ubi_bdev_head, tailq) {
        if (ubi_bdev->base_bdev_info.bdev == base_bdev) {
            *_ubi_bdev = ubi_bdev;
            *_base_info = &ubi_bdev->base_bdev_info;
            return true;
        }
    }

    return false;
}

static void store_littleendian_shortint(uint16_t n, uint8_t *mem) {
    mem[0] = (n & 0xff);
    mem[1] = (n >> 8);
}

static uint16_t load_littleendian_shortint(uint8_t *mem) {
    uint16_t result = mem[0] | (((uint16_t)mem[1]) << 8);
    return result;
}

static void ubi_set_version(struct ubi_metadata *metadata, uint16_t major,
                            uint16_t minor) {
    store_littleendian_shortint(major, metadata->versionMajor);
    store_littleendian_shortint(minor, metadata->versionMinor);
}

static void ubi_get_version(struct ubi_metadata *metadata, uint16_t *major,
                            uint16_t *minor) {
    *major = load_littleendian_shortint(metadata->versionMajor);
    *minor = load_littleendian_shortint(metadata->versionMinor);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_ubi)
