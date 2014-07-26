#include "internal.h"
#include "bucketconfig/clconfig.h"

struct lcb_bootstrap_st {
    clconfig_listener listener;
    lcb_t parent;
    lcbio_pTIMER tm;
    hrtime_t last_refresh;

    /** Flag set if we've already bootstrapped */
    int bootstrapped;
    unsigned errcounter;
};

#define LOGARGS(instance, lvl) instance->settings, "bootstrap", LCB_LOG_##lvl, __FILE__, __LINE__

static void async_step_callback(clconfig_listener*,clconfig_event_t,clconfig_info*);
static void initial_bootstrap_error(lcb_t, lcb_error_t,const char*);

/**
 * This function is where the configuration actually takes place. We ensure
 * in other functions that this is only ever called directly from an event
 * loop stack frame (or one of the small mini functions here) so that we
 * don't accidentally end up destroying resources underneath us.
 */
static void
config_callback(clconfig_listener *listener, clconfig_event_t event,
    clconfig_info *info)
{
    struct lcb_bootstrap_st *bs = (struct lcb_bootstrap_st *)listener;
    lcb_t instance = bs->parent;

    if (event != CLCONFIG_EVENT_GOT_NEW_CONFIG) {
        if (event == CLCONFIG_EVENT_PROVIDERS_CYCLED) {
            if (!LCBT_VBCONFIG(instance)) {
                initial_bootstrap_error(
                    instance, LCB_ERROR, "No more bootstrap providers remain");
            }
        }
        return;
    }

    instance->last_error = LCB_SUCCESS;
    /** Ensure we're not called directly twice again */
    listener->callback = async_step_callback;
    lcbio_timer_disarm(bs->tm);

    lcb_log(LOGARGS(instance, DEBUG), "Instance configured!");

    if (instance->type != LCB_TYPE_CLUSTER) {
        lcb_update_vbconfig(instance, info);
    }

    if (!bs->bootstrapped) {
        bs->bootstrapped = 1;
        lcb_aspend_del(&instance->pendops, LCB_PENDTYPE_COUNTER, NULL);

        if (instance->type == LCB_TYPE_BUCKET &&
                instance->dist_type == LCBVB_DIST_KETAMA &&
                instance->cur_configinfo->origin != LCB_CLCONFIG_MCRAW) {

            lcb_log(LOGARGS(instance, INFO), "Reverting to HTTP Config for memcached buckets");
            instance->settings->bc_http_stream_time = -1;
            lcb_confmon_set_provider_active(
                instance->confmon, LCB_CLCONFIG_HTTP, 1);
            lcb_confmon_set_provider_active(
                instance->confmon, LCB_CLCONFIG_CCCP, 0);
        }
        instance->callbacks.bootstrap(instance, LCB_SUCCESS);
    }

    lcb_maybe_breakout(instance);
}


static void
initial_bootstrap_error(lcb_t instance, lcb_error_t err, const char *errinfo)
{
    struct lcb_bootstrap_st *bs = instance->bootstrap;

    instance->last_error = lcb_confmon_last_error(instance->confmon);
    if (instance->last_error == LCB_SUCCESS) {
        instance->last_error = err;
    }
    instance->callbacks.error(instance, instance->last_error, errinfo);
    lcb_log(LOGARGS(instance, ERR), "Failed to bootstrap client=%p. Code=0x%x, Message=%s", (void *)instance, err, errinfo);
    lcbio_timer_disarm(bs->tm);

    instance->callbacks.bootstrap(instance, instance->last_error);

    lcb_aspend_del(&instance->pendops, LCB_PENDTYPE_COUNTER, NULL);
    lcb_maybe_breakout(instance);
}

/**
 * This it the initial bootstrap timeout handler. This timeout pins down the
 * instance. It is only scheduled during the initial bootstrap and is only
 * triggered if the initial bootstrap fails to configure in time.
 */
static void initial_timeout(void *arg)
{
    struct lcb_bootstrap_st *bs = arg;
    initial_bootstrap_error(bs->parent, LCB_ETIMEDOUT, "Failed to bootstrap in time");
}

/**
 * Proxy async call to config_callback
 */
static void async_refresh(void *arg)
{
    /** Get the best configuration and run stuff.. */
    struct lcb_bootstrap_st *bs = arg;
    clconfig_info *info;

    info = lcb_confmon_get_config(bs->parent->confmon);
    config_callback(&bs->listener, CLCONFIG_EVENT_GOT_NEW_CONFIG, info);
}

/**
 * set_next listener callback which schedules an async call to our config
 * callback.
 */
static void
async_step_callback(clconfig_listener *listener, clconfig_event_t event,
    clconfig_info *info)
{
    struct lcb_bootstrap_st *bs = (struct lcb_bootstrap_st *)listener;

    if (event != CLCONFIG_EVENT_GOT_NEW_CONFIG) {
        return;
    }

    if (lcbio_timer_armed(bs->tm) && lcbio_timer_get_target(bs->tm) == async_refresh) {
        lcb_log(LOGARGS(bs->parent, DEBUG), "Timer already present..");
        return;
    }

    lcb_log(LOGARGS(bs->parent, INFO), "Got async step callback..");
    lcbio_timer_set_target(bs->tm, async_refresh);
    lcbio_async_signal(bs->tm);
    (void)info;
}

static lcb_error_t bootstrap_common(lcb_t instance, int initial)
{
    struct lcb_bootstrap_st *bs = instance->bootstrap;

    if (bs && lcb_confmon_is_refreshing(instance->confmon)) {
        return LCB_SUCCESS;
    }

    if (!bs) {
        bs = calloc(1, sizeof(*instance->bootstrap));
        if (!bs) {
            return LCB_CLIENT_ENOMEM;
        }

        bs->tm = lcbio_timer_new(instance->iotable, bs, initial_timeout);
        instance->bootstrap = bs;
        bs->parent = instance;
        lcb_confmon_add_listener(instance->confmon, &bs->listener);
    }

    bs->last_refresh = gethrtime();

    if (initial) {
        bs->listener.callback = config_callback;
        lcbio_timer_set_target(bs->tm, initial_timeout);
        lcbio_timer_rearm(bs->tm, LCBT_SETTING(instance, config_timeout));
        lcb_aspend_add(&instance->pendops, LCB_PENDTYPE_COUNTER, NULL);

    } else {
        /** No initial timer */
        bs->listener.callback = async_step_callback;
    }

    return lcb_confmon_start(instance->confmon);
}

lcb_error_t lcb_bootstrap_initial(lcb_t instance)
{
    lcb_confmon_prepare(instance->confmon);
    return bootstrap_common(instance, 1);
}

lcb_error_t lcb_bootstrap_refresh(lcb_t instance)
{
    return bootstrap_common(instance, 0);
}

void lcb_bootstrap_errcount_incr(lcb_t instance)
{
    lcb_SIZE errthresh;
    struct lcb_bootstrap_st *bs = instance->bootstrap;
    hrtime_t now = gethrtime(), next_refresh_time;

    errthresh = LCBT_SETTING(instance, weird_things_threshold);
    bs->errcounter++;
    next_refresh_time = instance->bootstrap->last_refresh;
    next_refresh_time += LCB_US2NS(LCBT_SETTING(instance, weird_things_delay));

    if (now < next_refresh_time && bs->errcounter < errthresh) {
        lcb_log(LOGARGS(instance, INFO),
            "Not requesting a config refresh because of throttling parameters. Next refresh possible in %ums or %u errors. "
            "See LCB_CNTL_CONFDELAY_THRESH and LCB_CNTL_CONFERRTHRESH to modify the throttling settings",
            LCB_NS2US(next_refresh_time-now)/1000, (unsigned)errthresh-bs->errcounter);
        return;
    }

    bs->errcounter = 0;
    lcb_bootstrap_refresh(instance);
}

void lcb_bootstrap_destroy(lcb_t instance)
{
    struct lcb_bootstrap_st *bs = instance->bootstrap;
    if (!bs) {
        return;
    }
    if (bs->tm) {
        lcbio_timer_destroy(bs->tm);
    }

    lcb_confmon_remove_listener(instance->confmon, &bs->listener);
    free(bs);
    instance->bootstrap = NULL;
}

LIBCOUCHBASE_API
lcb_error_t
lcb_get_bootstrap_status(lcb_t instance)
{
    if (instance->cur_configinfo) {
        return LCB_SUCCESS;
    }
    if (instance->last_error != LCB_SUCCESS) {
        return instance->last_error;
    }
    if (instance->type == LCB_TYPE_CLUSTER) {
        lcbio_SOCKET *restconn = lcb_confmon_get_rest_connection(instance->confmon);
        if (restconn) {
            return LCB_SUCCESS;
        }
    }
    return LCB_ERROR;
}

LIBCOUCHBASE_API
void
lcb_refresh_config(lcb_t instance)
{
    lcb_bootstrap_refresh(instance);
}
