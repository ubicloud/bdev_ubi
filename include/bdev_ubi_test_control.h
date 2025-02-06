#ifndef BDEV_UBI_TEST_CONTROL_H
#define BDEV_UBI_TEST_CONTROL_H

#include <stdbool.h>

/* bdev_ubi.c */
void ubi_fail_create_channel_for_metadata_read(bool fail);
void ubi_fail_calloc_ubi_bdev(bool fail);

/* bdev_ubi_io_channel.c */
extern void ubi_io_channel_fail_register_poller(bool fail);
extern void ubi_io_channel_fail_create_base_ch(bool fail);
extern void ubi_io_channel_fail_image_file_open(bool fail);
extern void ubi_io_channel_fail_uring_queue_init(bool fail);

#endif /* BDEV_UBI_TEST_CONTROL_H */
