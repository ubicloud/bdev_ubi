
#include "bdev_ubi_internal.h"

#include "spdk/likely.h"
#include "spdk/log.h"

/*
 * Static function forward declarations
 */
static void ubi_trigger_snapshot(struct spdk_io_channel_iter *i);
static void ubi_snapshot_done(struct spdk_io_channel_iter *i, int status);
static void ubi_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
                         void *event_ctx);
static void ubi_release_snapshot_bdev(struct ubi_io_channel *ubi_ch);

const int SNAPSHOT_TRIGGERED = 10101;

struct ubi_snapshot_state {
    spdk_snapshot_ubi_complete cb_fn;
    void *cb_arg;
    char target[1024];
};

void bdev_ubi_snapshot(const char *source, const char *target,
                       spdk_snapshot_ubi_complete cb_fn, void *cb_arg) {
    struct spdk_bdev_desc *desc;
    int rc = spdk_bdev_open_ext(source, true, ubi_event_cb, NULL, &desc);
    if (rc != 0) {
        SPDK_ERRLOG("Could not open bdev %s: %s\n", source, spdk_strerror(-rc));
        cb_fn(cb_arg, rc);
        return;
    }

    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
    struct ubi_bdev *io_device = bdev->ctxt;

    struct ubi_snapshot_state *state = calloc(1, sizeof(*state));
    if (spdk_unlikely(state == NULL)) {
        SPDK_ERRLOG("Could not allocate memory for snapshot state\n");
        spdk_bdev_close(desc);
        cb_fn(cb_arg, -ENOMEM);
        return;
    }

    state->cb_fn = cb_fn;
    state->cb_arg = cb_arg;
    strncpy(state->target, target, sizeof(state->target) - 1);
    state->target[sizeof(state->target) - 1] = '\0';

    spdk_for_each_channel(io_device, ubi_trigger_snapshot, state, ubi_snapshot_done);
}

static void ubi_trigger_snapshot(struct spdk_io_channel_iter *i) {
    int rc;
    SPDK_WARNLOG("Triggering snapshot\n");
    struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
    struct ubi_io_channel *ubi_ch = spdk_io_channel_get_ctx(ch);
    struct ubi_snapshot_state *state = spdk_io_channel_iter_get_ctx(i);
    ubi_ch->snapshotting = true;
    rc = configure_base_bdev(state->target, true, &ubi_ch->snapshot_base_info);
    if (rc != 0) {
        SPDK_ERRLOG("Could not configure base bdev %s: %s\n", state->target,
                    spdk_strerror(-rc));
        spdk_for_each_channel_continue(i, rc);
        return;
    }

    SPDK_WARNLOG("Releasing snapshot bdev\n");
    ubi_ch->snapshotting = false;
    ubi_release_snapshot_bdev(ubi_ch);

    spdk_for_each_channel_continue(i, SNAPSHOT_TRIGGERED);
}

static void ubi_release_snapshot_bdev(struct ubi_io_channel *ubi_ch) {
    if (ubi_ch->snapshot_base_info.bdev != NULL) {
        spdk_bdev_module_release_bdev(ubi_ch->snapshot_base_info.bdev);
        spdk_bdev_close(ubi_ch->snapshot_base_info.desc);
        ubi_ch->snapshot_base_info.bdev = NULL;
    }
}

static void ubi_snapshot_done(struct spdk_io_channel_iter *i, int status) {
    struct ubi_snapshot_state *state = spdk_io_channel_iter_get_ctx(i);
    if (status != SNAPSHOT_TRIGGERED) {
        SPDK_ERRLOG("Could not trigger snapshot: %s\n", spdk_strerror(-status));
        state->cb_fn(state->cb_arg, -1);
    } else {
        state->cb_fn(state->cb_arg, 0);
    }
    free(state);
}

static void ubi_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
                         void *event_ctx) {
    SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}
