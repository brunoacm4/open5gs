/*
 * PANIC: the event loop of a box (see box.h).
 *
 * Whose event is it?
 *   - private events: by number, each part has its own range;
 *   - SBI requests: by the path of the URI;
 *   - SBI responses and client-wait timers: by the requester of the transaction;
 *   - NRF management (responses and timers): the first part.
 */

#include "ogs-app.h"
#include "box.h"

int __box_log_domain;

static const box_part_t *parts;
static int num_of_parts;

static ogs_thread_t *thread;
static ogs_timer_t *t_termination_holding;

static const box_part_t *part_of_xact(void *data)
{
    ogs_pool_id_t xact_id = OGS_POINTER_TO_UINT(data);
    ogs_sbi_xact_t *xact = NULL;
    int i;

    if (xact_id >= OGS_MIN_POOL_ID && xact_id <= OGS_MAX_POOL_ID)
        xact = ogs_sbi_xact_find_by_id(xact_id);

    if (xact) {
        for (i = 0; i < num_of_parts; i++) {
            if (parts[i].nf_type == xact->requester_nf_type)
                return &parts[i];
        }
    }

    /* The transaction is gone: any state machine reports it the same way */
    return &parts[0];
}

static const box_part_t *part_of_event(ogs_event_t *e)
{
    int i;

    switch (e->id) {
    case OGS_EVENT_SBI_SERVER:
        for (i = 0; i < num_of_parts; i++) {
            if (parts[i].uri_prefix && e->sbi.request->h.uri &&
                strncmp(e->sbi.request->h.uri, parts[i].uri_prefix,
                    strlen(parts[i].uri_prefix)) == 0)
                return &parts[i];
        }
        return &parts[0];

    case OGS_EVENT_SBI_CLIENT:
        /* On NF management responses, data is not a transaction */
        if (e->sbi.response->h.uri &&
            strstr(e->sbi.response->h.uri, "/nnrf-nfm/"))
            return &parts[0];
        return part_of_xact(e->sbi.data);

    case OGS_EVENT_SBI_TIMER:
        if (e->timer_id == OGS_TIMER_SBI_CLIENT_WAIT)
            return part_of_xact(e->sbi.data);
        return &parts[0];

    default:
        for (i = 0; i < num_of_parts; i++) {
            if (e->id > parts[i].event_base() && e->id < parts[i].event_top())
                return &parts[i];
        }
    }

    ogs_fatal("No part for event [%d]", e->id);
    ogs_assert_if_reached();
    return NULL;
}

static void box_main(void *data)
{
    int i, rv;

    for (i = 0; i < num_of_parts; i++)
        parts[i].fsm_init();

    for ( ;; ) {
        ogs_pollset_poll(ogs_app()->pollset,
                ogs_timer_mgr_next(ogs_app()->timer_mgr));

        /* As in amf_main(): expire the timers before using the event queue */
        ogs_timer_mgr_expire(ogs_app()->timer_mgr);

        for ( ;; ) {
            ogs_event_t *e = NULL;

            rv = ogs_queue_trypop(ogs_app()->queue, (void**)&e);
            ogs_assert(rv != OGS_ERROR);

            if (rv == OGS_DONE)
                goto done;

            if (rv == OGS_RETRY)
                break;

            ogs_assert(e);
            part_of_event(e)->dispatch(e);
            ogs_event_free(e);
        }
    }
done:

    for (i = num_of_parts - 1; i >= 0; i--)
        parts[i].fsm_fini();
}

int box_start(const box_part_t *table, int num)
{
    int i;

    ogs_assert(table);
    ogs_assert(num > 0);

    parts = table;
    num_of_parts = num;

    /* The ranges are the only thing that tells private events apart */
    for (i = 1; i < num_of_parts; i++)
        ogs_assert(parts[i - 1].event_top() <= parts[i].event_base());

    thread = ogs_thread_create(box_main, NULL);
    if (!thread) return OGS_ERROR;

    return OGS_OK;
}

void box_stop(void)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL;

    /* Sending NF Instance De-registration to NRF */
    ogs_list_for_each(&ogs_sbi_self()->nf_instance_list, nf_instance)
        ogs_sbi_nf_fsm_fini(nf_instance);

    /* Gracefully shutdown the server by sending GOAWAY to each session. */
    ogs_sbi_server_graceful_shutdown_all();

    /* Starting holding timer */
    t_termination_holding = ogs_timer_add(ogs_app()->timer_mgr, NULL, NULL);
    ogs_assert(t_termination_holding);
    ogs_timer_start(t_termination_holding, ogs_time_from_msec(300));

    /* Sending termination event to the queue */
    ogs_queue_term(ogs_app()->queue);
    ogs_pollset_notify(ogs_app()->pollset);

    ogs_thread_destroy(thread);
    ogs_timer_delete(t_termination_holding);
}
