
#include "bdev_ubi_internal.h"

#include "spdk/likely.h"
#include "spdk/log.h"

/*
 * Static function forward declarations
 */
static void write_stripe_io_completion(struct spdk_bdev_io *bdev_io, bool success,
                                       void *cb_arg);
static void ubi_fail_stripe_fetch(struct stripe_fetch *stripe_fetch);

void ubi_start_fetch_stripe(struct ubi_io_channel *ch,
                            struct stripe_fetch *stripe_fetch) {
    struct ubi_bdev *ubi_bdev = ch->ubi_bdev;
    struct io_uring *ring = &ch->image_file_ring;
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    uint32_t stripe_idx = stripe_fetch->stripe_idx;
    uint32_t alignment = ubi_bdev->alignment_bytes;

    uint64_t offset = ubi_bdev->stripe_size_kb * 1024L * stripe_idx;
    uint32_t nbytes = ubi_bdev->stripe_size_kb * 1024L;

    uint32_t alignment_offset =
        alignment - ((uint64_t)stripe_fetch->buf & (alignment - 1));
    stripe_fetch->buf_aligned = stripe_fetch->buf + alignment_offset;

    io_uring_prep_read(sqe, ch->image_file_fd, stripe_fetch->buf_aligned, nbytes, offset);
    io_uring_sqe_set_data(sqe, stripe_fetch);

    int ret = io_uring_submit(ring);
    if (ret < 0) {
        UBI_ERRLOG(ubi_bdev, "fetching stripe %d failed, io_uring_submit error: %s\n",
                   stripe_idx, strerror(-ret));
        ubi_fail_stripe_fetch(stripe_fetch);
    }
}

int ubi_complete_fetch_stripe(struct ubi_io_channel *ch,
                              struct stripe_fetch *stripe_fetch, int res) {
    uint64_t offset = ch->ubi_bdev->stripe_size_kb * 1024L * stripe_fetch->stripe_idx;
    uint32_t nbytes = ch->ubi_bdev->stripe_size_kb * 1024L;

    if (res < 0) {
        UBI_ERRLOG(ch->ubi_bdev,
                   "fetching stripe %d failed while checking cqe->res: %s\n",
                   stripe_fetch->stripe_idx, strerror(-res));
        ubi_fail_stripe_fetch(stripe_fetch);
        return -1;
    }

    /*
     * Now that we have read the stripe and have it in memory, write it to the
     * base bdev.
     */
    struct ubi_bdev *ubi_bdev = ch->ubi_bdev;
    struct ubi_base_bdev_info *base_info = &ubi_bdev->base_bdev_info;

    int ret = spdk_bdev_write(base_info->desc, ch->base_channel,
                              stripe_fetch->buf_aligned, offset + UBI_METADATA_SIZE,
                              nbytes, write_stripe_io_completion, stripe_fetch);
    if (ret != 0) {
        UBI_ERRLOG(ch->ubi_bdev, "fetching stripe %d failed, spdk_bdev_write error: %s\n",
                   stripe_fetch->stripe_idx, strerror(-ret));
        ubi_fail_stripe_fetch(stripe_fetch);
        return -1;
    }

    return 1;
}

static void write_stripe_io_completion(struct spdk_bdev_io *bdev_io, bool success,
                                       void *cb_arg) {
    spdk_bdev_free_io(bdev_io);

    struct stripe_fetch *stripe_fetch = cb_arg;
    ubi_set_stripe_status(stripe_fetch->ubi_bdev, stripe_fetch->stripe_idx,
                          STRIPE_FETCHED);
    stripe_fetch->ubi_bdev->stripes_fetched++;
    stripe_fetch->active = false;
}

static void ubi_fail_stripe_fetch(struct stripe_fetch *stripe_fetch) {
    ubi_set_stripe_status(stripe_fetch->ubi_bdev, stripe_fetch->stripe_idx,
                          STRIPE_FAILED);
    stripe_fetch->active = false;
}

void enqueue_stripe(struct ubi_io_channel *ch, int stripe_idx) {
    ch->stripe_fetch_queue.entries[ch->stripe_fetch_queue.tail] = stripe_idx;
    ch->stripe_fetch_queue.tail =
        (ch->stripe_fetch_queue.tail + 1) & (UBI_FETCH_QUEUE_SIZE - 1);
}

int dequeue_stripe(struct ubi_io_channel *ch) {
    int stripe_idx = ch->stripe_fetch_queue.entries[ch->stripe_fetch_queue.head];
    ch->stripe_fetch_queue.head =
        (ch->stripe_fetch_queue.head + 1) & (UBI_FETCH_QUEUE_SIZE - 1);
    return stripe_idx;
}

bool stripe_queue_empty(struct ubi_io_channel *ch) {
    return ch->stripe_fetch_queue.head == ch->stripe_fetch_queue.tail;
}

enum stripe_status ubi_get_stripe_status(struct ubi_bdev *ubi_bdev, int index) {
    return ubi_bdev->stripe_status[index];
}

void ubi_set_stripe_status(struct ubi_bdev *ubi_bdev, int index,
                           enum stripe_status status) {
    ubi_bdev->stripe_status[index] = status;

    if (status == STRIPE_FETCHED) {
        ubi_bdev->metadata.stripe_headers[index][0] = 1;
    }
}
