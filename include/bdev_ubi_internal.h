#ifndef BDEV_UBI_INTERNAL_H
#define BDEV_UBI_INTERNAL_H

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/json.h"
#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/thread.h"

#include "bdev_ubi.h"

#include <liburing.h>

#define UBI_METADATA_SIZE 8388608

// support images upto 1TB = 2^40 (assuming 1MB stripe size)
#define UBI_MAX_STRIPES (1024 * 1024)
#define UBI_STRIPE_SIZE_MAX 8

#define UBI_PATH_LEN 1024

#define UBI_MAGIC "BDEV_UBI"
#define UBI_MAGIC_SIZE 9
#define UBI_VERSION_MAJOR 0
#define UBI_VERSION_MINOR 1

#define UBI_MAX_ACTIVE_STRIPE_FETCHES 8
#define UBI_FETCH_QUEUE_SIZE 32768

/*
 * On-disk metadata for a ubi bdev.
 */
struct ubi_metadata {
    uint8_t magic[UBI_MAGIC_SIZE];

    /* Parsed as little-endian 16-bit integers. */
    uint8_t versionMajor[2];
    uint8_t versionMinor[2];

    uint8_t stripe_size_mb;

    /*
     * Currently stripe_headers[i] will be either 0 or 1, but reserve 31 more
     * bits per stripe for future extension.
     */
    uint8_t stripe_headers[UBI_MAX_STRIPES][4];

    /* Unused space reserved for future extension. */
    uint8_t padding[UBI_METADATA_SIZE - UBI_MAGIC_SIZE - UBI_MAX_STRIPES * 4 - 5];
};

/*
 * State we need to keep for a single base bdev.
 */
struct ubi_base_bdev_info {
    struct spdk_bdev *bdev;
    struct spdk_bdev_desc *desc;
};

/*
 * Runtime status of a stripe.
 */
enum stripe_status {
    STRIPE_NOT_FETCHED = 0,
    STRIPE_INFLIGHT,
    STRIPE_FAILED,
    STRIPE_FETCHED
};

/*
 * Block device's state. ubi_create creates and sets up a ubi_bdev.
 * ubi_bdev->bdev is registered with spdk. When registering, a pointer to
 * the ubi_bdev is saved in "bdev.ctxt".
 */
struct ubi_bdev {
    struct spdk_bdev bdev;

    struct ubi_base_bdev_info base_bdev_info;

    char image_path[UBI_PATH_LEN];
    uint32_t stripe_size_mb;
    uint32_t stripe_block_count;
    uint32_t stripe_shift;
    uint32_t data_offset_blocks;
    uint64_t image_block_count;
    bool no_sync;

    enum stripe_status stripe_status[UBI_MAX_STRIPES];

    struct ubi_metadata metadata;
    uint64_t stripes_fetched;
    uint64_t stripes_flushed;

    /*
     * Thread where ubi_bdev was initialized. It's essential to close the base
     * bdev in the same thread in which it was opened.
     */
    struct spdk_thread *thread;

    /* queue pointer */
    TAILQ_ENTRY(ubi_bdev) tailq;
};

/*
 * State for a stripe fetch operation.
 */
struct stripe_fetch {
    /* Is this currently used for an ongoing stripe fetch? */
    bool active;

    /* Which stripe are we fetching? */
    uint32_t stripe_idx;

    /* Where will the data be stored at? */
    uint8_t buf[1024 * 1024 * UBI_STRIPE_SIZE_MAX];

    struct ubi_bdev *ubi_bdev;
};

/*
 * Per thread state for ubi bdev.
 */
struct ubi_io_channel {
    struct ubi_bdev *ubi_bdev;
    struct spdk_poller *poller;
    struct spdk_io_channel *base_channel;

    /*
     * Stripe fetches are initially queued in "stripe_fetch_queue". During each
     * I/O poller iteration, if there's an available spot in stripe_fetches, a
     * stripe fetch is dequeued and initiated.
     */
    struct stripe_fetch stripe_fetches[UBI_MAX_ACTIVE_STRIPE_FETCHES];

    struct {
        uint32_t entries[UBI_FETCH_QUEUE_SIZE];
        uint32_t head;
        uint32_t tail;
    } stripe_fetch_queue;

    /* io_uring stuff */
    int image_file_fd;
    struct io_uring image_file_ring;

    /* queue pointer */
    TAILQ_HEAD(, spdk_bdev_io) io;
};

/*
 * per I/O operation state.
 */
struct ubi_bdev_io {
    struct ubi_bdev *ubi_bdev;
    struct ubi_io_channel *ubi_ch;

    uint64_t block_offset;
    uint64_t block_count;

    uint64_t stripes_fetched;
};

/* bdev_ubi_flush.c */
void ubi_submit_flush_request(struct ubi_bdev_io *ubi_io);

/* bdev_ubi_stripe.c */
void ubi_start_fetch_stripe(struct ubi_io_channel *base_ch,
                            struct stripe_fetch *stripe_fetch);
int ubi_complete_fetch_stripe(struct ubi_io_channel *ch);
void enqueue_stripe(struct ubi_io_channel *ch, int stripe_idx);
int dequeue_stripe(struct ubi_io_channel *ch);
bool stripe_queue_empty(struct ubi_io_channel *ch);
enum stripe_status ubi_get_stripe_status(struct ubi_bdev *ubi_bdev, int stripe_index);
void ubi_set_stripe_status(struct ubi_bdev *ubi_bdev, int index,
                           enum stripe_status status);

/* bdev_ubi_io_channel.c */
int ubi_create_channel_cb(void *io_device, void *ctx_buf);
void ubi_destroy_channel_cb(void *io_device, void *ctx_buf);

/* macros */
#define UBI_ERRLOG(ubi_bdev, format, ...)                                                \
    SPDK_ERRLOG("[%s] " format, ubi_bdev->bdev.name __VA_OPT__(, ) __VA_ARGS__)

#endif
