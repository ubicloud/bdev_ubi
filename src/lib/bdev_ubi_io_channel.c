
#include "bdev_ubi_internal.h"

#include "spdk/likely.h"
#include "spdk/log.h"

/*
 * Static function forward declarations
 */
static int ubi_io_poll(void *arg);
static int ubi_complete_image_io(struct ubi_io_channel *ch);
static int ubi_complete_read_from_image(struct ubi_io_channel *ch,
                                        struct ubi_bdev_io *ubi_io, int res);
static void get_buf_for_read_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
                                bool success);
static int ubi_submit_read_request(struct ubi_bdev_io *ubi_io);
static int ubi_submit_write_request(struct ubi_bdev_io *ubi_io);
static void ubi_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success,
                                 void *cb_arg);
static void ubi_complete_io(struct ubi_bdev_io *ubi_io, bool success);
static void ubi_init_ext_io_opts(struct spdk_bdev_io *bdev_io,
                                 struct spdk_bdev_ext_io_opts *opts);

/*
 * ubi_create_channel_cb is called when an I/O channel needs to be created. In
 * the VM world this can happen for example when VMM's firmware needs to use the
 * disk, or when the virtio device is initiated by the operating system.
 */
int ubi_create_channel_cb(void *io_device, void *ctx_buf) {
    struct ubi_bdev *ubi_bdev = io_device;
    struct ubi_io_channel *ch = ctx_buf;

    ch->ubi_bdev = ubi_bdev;
    TAILQ_INIT(&ch->io);
    ch->poller = SPDK_POLLER_REGISTER(ubi_io_poll, ch, 0);

    ch->base_channel = spdk_bdev_get_io_channel(ubi_bdev->base_bdev_info.desc);

    ch->stripe_fetch_queue.head = 0;
    ch->stripe_fetch_queue.tail = 0;

    for (int i = 0; i < UBI_MAX_ACTIVE_STRIPE_FETCHES; i++) {
        ch->stripe_fetches[i].active = false;
        ch->stripe_fetches[i].ubi_bdev = ubi_bdev;
    }

    int open_flags = O_RDONLY;
    if (ubi_bdev->directio)
        open_flags |= O_DIRECT;
    ch->image_file_fd = open(ubi_bdev->image_path, open_flags);
    if (ch->image_file_fd < 0) {
        UBI_ERRLOG(ubi_bdev, "could not open %s: %s\n", ubi_bdev->image_path,
                   strerror(errno));
        return -EINVAL;
    }

    struct io_uring_params io_uring_params;
    memset(&io_uring_params, 0, sizeof(io_uring_params));
    int rc = io_uring_queue_init(UBI_URING_QUEUE_SIZE, &ch->image_file_ring, 0);
    if (rc != 0) {
        UBI_ERRLOG(ubi_bdev, "Unable to setup io_uring: %s\n", strerror(-rc));
        return -EINVAL;
    }

    return 0;
}

/*
 * ubi_destroy_channel_cb when an I/O channel needs to be destroyed.
 */
void ubi_destroy_channel_cb(void *io_device, void *ctx_buf) {
    struct ubi_io_channel *ch = ctx_buf;
    spdk_poller_unregister(&ch->poller);

    if (close(ch->image_file_fd) != 0) {
        UBI_ERRLOG(ch->ubi_bdev, "Error closing file: %s\n", strerror(errno));
    }

    SPDK_NOTICELOG(
        "stats for %s: blocks read: %ld, blocks written: %ld, stripes_fetched: %ld\n",
        ch->ubi_bdev->bdev.name, ch->blocks_read, ch->blocks_written,
        ch->stripes_fetched);

    io_uring_queue_exit(&ch->image_file_ring);
    spdk_put_io_channel(ch->base_channel);
}

/*
 * ubi_io_poll is the poller function that is called regularly by SPDK.
 */
static int ubi_io_poll(void *arg) {
    struct ubi_io_channel *ch = arg;
    struct spdk_bdev_io *bdev_io;
    struct ubi_bdev *ubi_bdev = ch->ubi_bdev;

    bool queues_empty = TAILQ_EMPTY(&ch->io) && stripe_queue_empty(ch);
    int image_ios_completed = ubi_complete_image_io(ch);

    if (queues_empty && image_ios_completed < 1) {
        return SPDK_POLLER_IDLE;
    }

    if (queues_empty) {
        // no items in queues to process, but might have some more fetches to
        // finish.
        return SPDK_POLLER_BUSY;
    }

    /*
     * Create a list of free stripes.
     */
    int n_free_stripe_fetches = 0;
    int free_stripe_fetches[UBI_MAX_ACTIVE_STRIPE_FETCHES];
    for (int i = 0; i < UBI_MAX_ACTIVE_STRIPE_FETCHES; i++) {
        if (!ch->stripe_fetches[i].active) {
            free_stripe_fetches[n_free_stripe_fetches] = i;
            n_free_stripe_fetches++;
        }
    }

    /*
     * Dequeue and initiate stripe fetches.
     */
    int free_stripe_fetch_idx = 0;
    while (free_stripe_fetch_idx < n_free_stripe_fetches &&
           ch->stripe_fetch_queue.head != ch->stripe_fetch_queue.tail) {
        int stripe_idx = dequeue_stripe(ch);
        int assigned_stripe_fetch_idx = free_stripe_fetches[free_stripe_fetch_idx];
        struct stripe_fetch *stripe_fetch =
            &ch->stripe_fetches[assigned_stripe_fetch_idx];

        stripe_fetch->stripe_idx = stripe_idx;
        stripe_fetch->active = true;
        stripe_fetch->op.type = UBI_STRIPE_FETCH;
        ubi_start_fetch_stripe(ch, stripe_fetch);
        ch->stripes_fetched++;

        free_stripe_fetch_idx++;
    }

    /*
     * Dequeue and process I/O requests.
     */
    while (!TAILQ_EMPTY(&ch->io)) {
        bdev_io = TAILQ_FIRST(&ch->io);

        if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ &&
            ch->active_reads > UBI_MAX_CONCURRENT_READS)
            break;

        uint64_t start_block = bdev_io->u.bdev.offset_blocks;
        if (bdev_io->type != SPDK_BDEV_IO_TYPE_FLUSH &&
            start_block < ubi_bdev->image_block_count) {
            uint64_t stripe = start_block >> ubi_bdev->stripe_shift;
            enum stripe_status stripe_status = ubi_get_stripe_status(ubi_bdev, stripe);

            if (stripe_status == STRIPE_FAILED) {
                /*
                 * The attempt to fetch the stripe containing the block was
                 * unsuccessful. Dequeue it and mark it as failed.
                 */
                TAILQ_REMOVE(&ch->io, bdev_io, module_link);
                spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                continue;
            } else if (stripe_status == STRIPE_INFLIGHT) {
                /*
                 * The stripe containing the block is currently being fetched.
                 * Halt the loop to ensure I/O requests are addressed in the
                 * order they were received.
                 */
                break;
            } else if (stripe_status == STRIPE_NOT_FETCHED &&
                       (bdev_io->type != SPDK_BDEV_IO_TYPE_READ ||
                        ubi_bdev->copy_on_read)) {
                /*
                 * This is a programming error. Such a scenario should never
                 * arise. If an I/O request from the base image was enqueued,
                 * the fetching process should have commenced.
                 */
                UBI_ERRLOG(ch->ubi_bdev,
                           "BUG: I/O for block %lu enqueued, but stripe %lu "
                           "isn't enqueued.\n",
                           start_block, stripe);

                TAILQ_REMOVE(&ch->io, bdev_io, module_link);
                spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                continue;
            }
        }

        /*
         * At this point, the block is either beyond the base image size, or its
         * stripe has been fetched. Hence, dequeue and service it.
         */

        TAILQ_REMOVE(&ch->io, bdev_io, module_link);

        struct ubi_bdev_io *ubi_io = (struct ubi_bdev_io *)bdev_io->driver_ctx;
        ubi_io->ubi_bdev = ubi_bdev;
        ubi_io->ubi_ch = ch;
        ubi_io->block_offset = bdev_io->u.bdev.offset_blocks;
        ubi_io->block_count = bdev_io->u.bdev.num_blocks;
        ubi_io->op.type = UBI_BDEV_IO;

        switch (bdev_io->type) {
        case SPDK_BDEV_IO_TYPE_READ: {
            ch->active_reads++;
            ch->blocks_read += ubi_io->block_count;
            uint64_t len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
            spdk_bdev_io_get_buf(bdev_io, get_buf_for_read_cb, len);
            break;
        }
        case SPDK_BDEV_IO_TYPE_WRITE:
            ch->blocks_written += ubi_io->block_count;
            ubi_submit_write_request(ubi_io);
            break;
        case SPDK_BDEV_IO_TYPE_FLUSH:
            ubi_submit_flush_request(ubi_io);
            break;
        default:
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            break;
        }
    }

    if (ch->has_reads) {
        int ret = io_uring_submit(&ch->image_file_ring);
        if (ret < 0) {
            UBI_ERRLOG(ubi_bdev, "io_uring_submit failed: %d\n", ret);
        } else {
            ch->has_reads = 0;
        }
    }

    return SPDK_POLLER_BUSY;
}

static int ubi_complete_image_io(struct ubi_io_channel *ch) {
    struct io_uring *ring = &ch->image_file_ring;
    struct io_uring_cqe *cqe[64];

    int ret = io_uring_peek_batch_cqe(ring, cqe, 64);
    if (ret == -EAGAIN) {
        return 0;
    } else if (ret < 0) {
        UBI_ERRLOG(ch->ubi_bdev, "io_uring_peek_cqe: %s\n", strerror(-ret));
        return -1;
    }

    for (int i = 0; i < ret; i++) {
        struct ubi_io_op *op = io_uring_cqe_get_data(cqe[i]);

        switch (op->type) {
        case UBI_STRIPE_FETCH:
            ret = ubi_complete_fetch_stripe(ch, (struct stripe_fetch *)op, cqe[i]->res);
            break;
        case UBI_BDEV_IO:
            ret = ubi_complete_read_from_image(ch, (struct ubi_bdev_io *)op, cqe[i]->res);
            break;
        }

        /* Mark the completion as seen. */
        io_uring_cqe_seen(ring, cqe[i]);
    }

    return ret;
}

/*
 * get_buf_for_read_cb
 */
static void get_buf_for_read_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
                                bool success) {
    struct ubi_bdev_io *ubi_io = (struct ubi_bdev_io *)bdev_io->driver_ctx;
    struct ubi_bdev *ubi_bdev = ubi_io->ubi_bdev;

    if (!success) {
        ubi_complete_io(ubi_io, false);
        return;
    }

    uint64_t start_block = bdev_io->u.bdev.offset_blocks;
    uint64_t stripe = start_block >> ubi_bdev->stripe_shift;
    enum stripe_status stripe_status = ubi_get_stripe_status(ubi_bdev, stripe);

    if (stripe_status == STRIPE_FETCHED || start_block >= ubi_bdev->image_block_count) {
        int ret = ubi_submit_read_request(ubi_io);

        if (spdk_unlikely(ret != 0)) {
            ubi_complete_io(ubi_io, false);
        }
    } else {
        // read from base image.
        struct ubi_io_channel *ubi_ch = ubi_io->ubi_ch;
        struct io_uring *ring = &ubi_ch->image_file_ring;
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        uint64_t offset = start_block * ubi_bdev->bdev.blocklen;
        io_uring_prep_readv(sqe, ubi_ch->image_file_fd, bdev_io->u.bdev.iovs,
                            bdev_io->u.bdev.iovcnt, offset);
        io_uring_sqe_set_data(sqe, ubi_io);
        ubi_ch->has_reads++;
    }
}

static int ubi_complete_read_from_image(struct ubi_io_channel *ch,
                                        struct ubi_bdev_io *ubi_io, int res) {
    if (res < 0) {
        ubi_complete_io(ubi_io, false);
        return -1;
    }

    ubi_complete_io(ubi_io, true);
    return 1;
}

/*
 * ubi_submit_read_request processes a read I/O request. At this point the
 * stripe containing the address range for this I/O has been fetched, so
 * we can just redirect the I/O to the base bdev.
 */
static int ubi_submit_read_request(struct ubi_bdev_io *ubi_io) {
    struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(ubi_io);

    struct spdk_bdev_ext_io_opts io_opts;
    ubi_init_ext_io_opts(bdev_io, &io_opts);

    struct ubi_bdev *ubi_bdev = ubi_io->ubi_bdev;
    struct ubi_base_bdev_info *base_info = &ubi_bdev->base_bdev_info;
    struct spdk_io_channel *base_ch = ubi_io->ubi_ch->base_channel;

    uint64_t start_block = bdev_io->u.bdev.offset_blocks + ubi_bdev->data_offset_blocks;
    uint64_t num_blocks = bdev_io->u.bdev.num_blocks;
    int ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch, bdev_io->u.bdev.iovs,
                                         bdev_io->u.bdev.iovcnt, start_block, num_blocks,
                                         ubi_io_completion_cb, ubi_io, &io_opts);
    return ret;
}

/*
 * ubi_submit_write_request processes a write I/O request. At this point the
 * stripe containing the address range for this I/O has been fetched, so
 * we can just redirect the I/O to the base bdev.
 */
static int ubi_submit_write_request(struct ubi_bdev_io *ubi_io) {
    struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(ubi_io);

    struct spdk_bdev_ext_io_opts io_opts;
    ubi_init_ext_io_opts(bdev_io, &io_opts);

    struct ubi_bdev *ubi_bdev = ubi_io->ubi_bdev;

    uint64_t start_block = bdev_io->u.bdev.offset_blocks + ubi_bdev->data_offset_blocks;
    uint64_t num_blocks = bdev_io->u.bdev.num_blocks;

    struct ubi_base_bdev_info *base_info = &ubi_io->ubi_bdev->base_bdev_info;
    struct spdk_io_channel *base_ch = ubi_io->ubi_ch->base_channel;
    int ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch, bdev_io->u.bdev.iovs,
                                          bdev_io->u.bdev.iovcnt, start_block, num_blocks,
                                          ubi_io_completion_cb, ubi_io, &io_opts);
    return ret;
}

/*
 * ubi_io_completion_cb cleans up and marks the I/O request as completed.
 */
static void ubi_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success,
                                 void *cb_arg) {
    spdk_bdev_free_io(bdev_io);

    struct ubi_bdev_io *ubi_io = cb_arg;
    ubi_complete_io(ubi_io, success);
}

static void ubi_complete_io(struct ubi_bdev_io *ubi_io, bool success) {
    struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(ubi_io);

    if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ)
        ubi_io->ubi_ch->active_reads--;

    spdk_bdev_io_complete(bdev_io, success ? SPDK_BDEV_IO_STATUS_SUCCESS
                                           : SPDK_BDEV_IO_STATUS_FAILED);
}

/*
 * ubi_init_ext_io_opts fills in options for a request to the base bdev.
 */
static void ubi_init_ext_io_opts(struct spdk_bdev_io *bdev_io,
                                 struct spdk_bdev_ext_io_opts *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->size = sizeof(*opts);
    opts->memory_domain = bdev_io->u.bdev.memory_domain;
    opts->memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
    opts->metadata = bdev_io->u.bdev.md_buf;
}
