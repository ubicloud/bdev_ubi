#include "test_ubi.h"

pthread_mutex_t g_test_mutex;
pthread_cond_t g_test_cond;
struct spdk_thread *g_io_thread;
struct spdk_thread *g_init_thread;

void execute_spdk_function(spdk_msg_fn fn, void *arg) {
    pthread_mutex_lock(&g_test_mutex);
    spdk_thread_send_msg(g_io_thread, fn, arg);
    pthread_cond_wait(&g_test_cond, &g_test_mutex);
    pthread_mutex_unlock(&g_test_mutex);
}

void execute_app_function(spdk_msg_fn fn, void *arg) {
    pthread_mutex_lock(&g_test_mutex);
    spdk_thread_send_msg(g_init_thread, fn, arg);
    pthread_cond_wait(&g_test_cond, &g_test_mutex);
    pthread_mutex_unlock(&g_test_mutex);
}

void wake_ut_thread(void) {
    pthread_mutex_lock(&g_test_mutex);
    pthread_cond_signal(&g_test_cond);
    pthread_mutex_unlock(&g_test_mutex);
}

void run_ut_thread(void *arg) {
    struct test_opts *opts = arg;
    g_io_thread = opts->io_thread;
    g_init_thread = opts->init_thread;
    int n_tests = 0, n_failures = 0;

    pthread_mutex_init(&g_test_mutex, NULL);
    pthread_cond_init(&g_test_cond, NULL);

    for (size_t i = 0; i < opts->n_bdevs; i++) {
        SPDK_NOTICELOG("Testing %s\n", opts->bdev_names[i]);
        test_bdev_io(opts->bdev_names[i], opts->image_path, &n_tests, &n_failures);
    }

    test_bdev_recreate(opts->free_base_bdev, opts->image_path, &n_tests, &n_failures);

    SPDK_NOTICELOG("Tests run: %u, failures: %u\n", n_tests, n_failures);

    execute_spdk_function(exit_io_thread, NULL);
    spdk_thread_send_msg(opts->init_thread, stop_init_thread,
                         n_failures ? (void *)0x1 : NULL);
    spdk_thread_exit(spdk_get_thread());
}
