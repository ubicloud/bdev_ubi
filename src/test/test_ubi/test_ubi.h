#ifndef TEST_UBI_H
#define TEST_UBI_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/event.h"
#include "spdk/fd.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vmd.h"
#include <stdio.h>

#include "bdev_ubi.h"

#define MAX_BLOCK_SIZE 4096
#define MAX_BDEVS 10

#define TEST_FREE_BASE_BDEV "free_base_bdev"
#define TEST_IMAGE_PATH "bin/test/test_image.raw"
#define TEST_BASE_WITH_INVALID_MAGIC "base_with_invalid_magic"
#define TEST_TOO_SMALL_BASE "too_small_base"

struct bdev_desc_ch_pair {
    struct spdk_bdev_desc *desc;
    struct spdk_io_channel *ch;
};

struct test_opts {
    char *bdev_names[MAX_BDEVS];
    int n_bdevs;

    struct spdk_thread *init_thread;
    struct spdk_thread *io_thread;
    struct spdk_thread *ut_thread;
};

/* init_thread.c */
struct ubi_create_request {
    struct spdk_ubi_bdev_opts opts;
    bool success;
};

struct ubi_delete_request {
    char *name;
    bool success;
};

extern void stop_init_thread(void *arg);
extern void init_thread_create_bdev_ubi(void *arg);
extern void init_thread_delete_bdev_ubi(void *arg);

/*
 * io_thread.c
 */
struct ubi_io_request {
    char buf[MAX_BLOCK_SIZE];
    uint64_t block_idx;
    struct bdev_desc_ch_pair *bdev;

    bool success;
};

extern void open_io_channel(void *arg);
extern void close_io_channel(void *arg);
extern void exit_io_thread(void *arg);
extern void io_thread_write(void *arg);
extern void io_thread_read(void *arg);
extern void io_thread_flush(void *arg);

/*
 * ut_thread.c
 */
extern void execute_spdk_function(spdk_msg_fn fn, void *arg);
void execute_app_function(spdk_msg_fn fn, void *arg);
extern void wake_ut_thread(void);
extern void run_ut_thread(void *arg);

/*
 * common.c
 */
extern bool verify_create(const char *base_bdev, const char *image_path,
                          const char *bdev_name);
extern bool verify_delete(const char *bdev_name);
extern bool open_bdev_and_ch(const char *bdev_name, struct bdev_desc_ch_pair *bdev);
extern bool close_bdev_and_ch(struct bdev_desc_ch_pair *bdev);

/*
 * tests
 */
extern void test_bdev_io(const char *bdev_name, int *n_tests, int *n_failures);
extern bool test_bdev_recreate(void);
extern bool test_bdev_create_errors(void);
extern bool test_write_config(void);
extern bool test_io_channel_create_errors(void);

#endif
