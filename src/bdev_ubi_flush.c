
#include "bdev_ubi_internal.h"

#include "spdk/likely.h"
#include "spdk/log.h"

/*
 * Static function forward declarations
 */
static void ubi_data_flush_completion(struct spdk_bdev_io *bdev_io, bool success,
                                      void *cb_arg);
static void ubi_metadata_write_completion(struct spdk_bdev_io *bdev_io, bool success,
                                          void *cb_arg);
static void ubi_metadata_flush_completion(struct spdk_bdev_io *bdev_io, bool success,
                                          void *cb_arg);

/*
 * To process a flush (aka sync) request for a specified block range, the
 * data is first flushed to the base bdev. If metadata is not dirty, the
 * I/O request is marked as completed.
 *
 * If metadata is dirty, it's first written and subsequently flushed to
 * the base bdev. Then the I/O request is marked as completed.
 */

void ubi_submit_flush_request(struct ubi_bdev_io *ubi_io) {
    struct ubi_bdev *ubi_bdev = ubi_io->ubi_bdev;
    if (ubi_bdev->no_sync) {
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(ubi_io), SPDK_BDEV_IO_STATUS_SUCCESS);
        return;
    }

    uint64_t start_block = ubi_io->block_offset + ubi_io->ubi_bdev->data_offset_blocks;
    uint64_t num_blocks = ubi_io->block_count;

    struct ubi_base_bdev_info *base_info = &ubi_bdev->base_bdev_info;
    struct spdk_io_channel *base_ch = ubi_io->ubi_ch->base_channel;
    int ret = spdk_bdev_flush_blocks(base_info->desc, base_ch, start_block, num_blocks,
                                     ubi_data_flush_completion, ubi_io);
    if (ret) {
        UBI_ERRLOG(ubi_io->ubi_bdev,
                   "flush (start: %lu, len: %lu) failed, data flush error: %s\n",
                   start_block, num_blocks, strerror(-ret));
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(ubi_io), SPDK_BDEV_IO_STATUS_FAILED);
    }
}

static void ubi_data_flush_completion(struct spdk_bdev_io *bdev_io, bool success,
                                      void *cb_arg) {
    struct ubi_bdev_io *ubi_io = cb_arg;
    struct ubi_bdev *ubi_bdev = ubi_io->ubi_bdev;

    spdk_bdev_free_io(bdev_io);

    if (!success) {
        UBI_ERRLOG(ubi_io->ubi_bdev,
                   "flush (start: %lu, len: %lu) failed (data flush failure).\n",
                   ubi_io->block_offset, ubi_io->block_count);
        return;
    }

    if (ubi_bdev->stripes_fetched == ubi_bdev->stripes_flushed) {
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(ubi_io), SPDK_BDEV_IO_STATUS_SUCCESS);
        return;
    }

    struct ubi_base_bdev_info *base_info = &ubi_bdev->base_bdev_info;
    struct spdk_io_channel *base_ch = ubi_io->ubi_ch->base_channel;

    uint32_t num_blocks = UBI_METADATA_SIZE / ubi_bdev->bdev.blocklen;
    ubi_io->stripes_fetched = ubi_bdev->stripes_fetched;
    int ret = spdk_bdev_write_blocks(base_info->desc, base_ch, &ubi_bdev->metadata, 0,
                                     num_blocks, ubi_metadata_write_completion, ubi_io);
    if (ret) {
        UBI_ERRLOG(ubi_io->ubi_bdev,
                   "flush (start: %lu, len: %lu) failed, metadata write error: %s.\n",
                   ubi_io->block_offset, ubi_io->block_count, strerror(-ret));
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(ubi_io), SPDK_BDEV_IO_STATUS_FAILED);
    }
}

static void ubi_metadata_write_completion(struct spdk_bdev_io *bdev_io, bool success,
                                          void *cb_arg) {
    struct ubi_bdev_io *ubi_io = cb_arg;
    struct ubi_bdev *ubi_bdev = ubi_io->ubi_bdev;
    spdk_bdev_free_io(bdev_io);

    if (!success) {
        UBI_ERRLOG(ubi_io->ubi_bdev,
                   "flush (start: %lu, len: %lu) failed (metadata write failure).\n",
                   ubi_io->block_offset, ubi_io->block_count);
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(ubi_io), SPDK_BDEV_IO_STATUS_FAILED);
        return;
    }

    struct ubi_base_bdev_info *base_info = &ubi_bdev->base_bdev_info;
    struct spdk_io_channel *base_ch = ubi_io->ubi_ch->base_channel;
    uint32_t num_blocks = sizeof(ubi_bdev->metadata) / ubi_bdev->bdev.blocklen;
    int ret = spdk_bdev_flush_blocks(base_info->desc, base_ch, 0, num_blocks,
                                     ubi_metadata_flush_completion, ubi_io);
    if (ret) {
        UBI_ERRLOG(ubi_io->ubi_bdev,
                   "flush failed (start: %lu, len: %lu), metadata flush error: %s.\n",
                   ubi_io->block_offset, ubi_io->block_count, strerror(-ret));
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(ubi_io), SPDK_BDEV_IO_STATUS_FAILED);
    }
}

static void ubi_metadata_flush_completion(struct spdk_bdev_io *bdev_io, bool success,
                                          void *cb_arg) {
    struct ubi_bdev_io *ubi_io = cb_arg;
    struct ubi_bdev *ubi_bdev = ubi_io->ubi_bdev;
    spdk_bdev_free_io(bdev_io);

    if (!success) {
        UBI_ERRLOG(ubi_io->ubi_bdev,
                   "flush (start: %lu, len: %lu) failed (metadata flush failure).\n",
                   ubi_io->block_offset, ubi_io->block_count);
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(ubi_io), SPDK_BDEV_IO_STATUS_FAILED);
        return;
    }

    if (ubi_io->stripes_fetched > ubi_bdev->stripes_flushed) {
        ubi_bdev->stripes_flushed = ubi_io->stripes_fetched;
    }

    spdk_bdev_io_complete(spdk_bdev_io_from_ctx(ubi_io), SPDK_BDEV_IO_STATUS_SUCCESS);
}
