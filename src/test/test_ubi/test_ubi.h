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

#define MAX_BLOCK_SIZE 4096
#define MAX_BDEVS 10

struct test_state {
    struct spdk_bdev_desc *bdev_desc;
    struct spdk_io_channel *ch;
    FILE *image_file;

    uint32_t blocklen;
    uint64_t blockcnt;
    uint64_t image_size;
    uint64_t n_image_blocks;

    uint32_t n_tests;
    uint32_t n_failures;
};

struct test_opts {
    char *bdev_names[MAX_BDEVS];
    int n_bdevs;

    struct spdk_thread *init_thread;
    struct spdk_thread *io_thread;
    struct spdk_thread *ut_thread;
};

/* init_thread.c */
extern void stop_init_thread(void *arg);

/*
 * io_thread.c
 */
struct ubi_io_request {
    char buf[MAX_BLOCK_SIZE];
    uint64_t block_idx;
    struct test_state *state;

    bool success;
};

extern void open_io_channel(void *arg);
extern void close_io_channel(void *arg);
extern void exit_io_thread(void *arg);
extern void ubi_bdev_write(void *arg);
extern void ubi_bdev_read(void *arg);
extern void ubi_bdev_flush(void *arg);

/*
 * ut_thread.c
 */
extern void wake_ut_thread(void);
extern void run_ut_thread(void *arg);

#endif
