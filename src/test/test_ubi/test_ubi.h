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

struct test_bdev {
    struct spdk_bdev_desc *desc;
    struct spdk_io_channel *ch;
};

struct test_opts {
    char *free_base_bdev;
    char *image_path;
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
    struct test_bdev *bdev;

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
 * tests
 */
extern void test_bdev_io(const char *bdev_name, const char *image_path, int *n_tests,
                         int *n_failures);
extern void test_bdev_recreate(const char *base_bdev, const char *image_path,
                               int *n_tests, int *n_failures);
#endif
