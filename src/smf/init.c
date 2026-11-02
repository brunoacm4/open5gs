/*
 * Copyright (C) 2019-2025 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "context.h"
#include "fd-path.h"
#include "gtp-path.h"
#include "pfcp-path.h"
#include "sbi-path.h"
#include "metrics.h"
#include "ogs-metrics.h"
#include "metrics/prometheus/json_pager.h"
#include "pdu-info.h"
#include "persistence.h"
#include "part.h"

static ogs_thread_t *thread;
static void smf_main(void *data);

static int initialized = 0;

int smf_initialize(void)
{
    int rv;

#define APP_NAME "smf"
    rv = ogs_app_parse_local_conf(APP_NAME);
    if (rv != OGS_OK) return rv;

    smf_metrics_init();

    ogs_sbi_context_init(OpenAPI_nf_type_SMF);

    rv = smf_part_context_init();
    if (rv != OGS_OK) return rv;

    rv = ogs_log_config_domain(
            ogs_app()->logger.domain, ogs_app()->logger.level);
    if (rv != OGS_OK) return rv;

    rv = ogs_sbi_context_parse_config(APP_NAME, "nrf", "scp");
    if (rv != OGS_OK) return rv;

    rv = ogs_metrics_context_parse_config(APP_NAME);
    if (rv != OGS_OK) return rv;

    rv = smf_part_config();
    if (rv != OGS_OK) return rv;

    /* PANIC: persistent context (phase 1b) in the MongoDB of db_uri */
    if (smf_part_uses_db()) {
        if (!ogs_app()->db_uri) {
            ogs_error("smf.persistence.enabled needs db_uri");
            return OGS_ERROR;
        }
        rv = ogs_dbi_init(ogs_app()->db_uri);
        if (rv != OGS_OK) return rv;
    }

    ogs_metrics_context_open(ogs_metrics_self());

    rv = smf_part_restore();
    if (rv != OGS_OK) return rv;

    rv = smf_part_open();
    if (rv != OGS_OK) return rv;

    rv = smf_sbi_open();
    if (rv != 0) return OGS_ERROR;

    thread = ogs_thread_create(smf_main, NULL);
    if (!thread) return OGS_ERROR;

    smf_part_register_eps();

    initialized = 1;

    return OGS_OK;
}

void smf_part_metrics_init(void)
{
    smf_metrics_specs_init();
}

void smf_part_metrics_final(void)
{
    smf_metrics_specs_final();
}

int smf_part_context_init(void)
{
    int rv;

    ogs_gtp_context_init(ogs_app()->pool.nf * OGS_MAX_NUM_OF_GTPU_RESOURCE);
    ogs_pfcp_context_init();

    smf_context_init();

    rv = ogs_gtp_xact_init();
    if (rv != OGS_OK) return rv;

    return ogs_pfcp_xact_init();
}

/*
 * Inside a box the SMF does not register in the NRF (self is another NF type).
 * This local instance is how the rest of the box finds it: the AMF looks in
 * its local list before asking the NRF, and matches on NF type.
 */
void smf_part_instance_add(void)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL;
    ogs_uuid_t uuid;
    char id[OGS_UUID_FORMATTED_LENGTH + 1];

    nf_instance = ogs_sbi_nf_instance_add();
    ogs_assert(nf_instance);

    ogs_uuid_get(&uuid);
    ogs_uuid_format(id, &uuid);
    ogs_sbi_nf_instance_set_id(nf_instance, id);
    ogs_sbi_nf_instance_set_type(nf_instance, OpenAPI_nf_type_SMF);
    ogs_sbi_nf_fsm_init(nf_instance);

    smf_sbi_build_profile(nf_instance);
    ogs_sbi_client_associate(nf_instance);

    smf_self()->nf_instance = nf_instance;
}

int smf_part_config(void)
{
    int rv;

    rv = ogs_gtp_context_parse_config(APP_NAME, "upf");
    if (rv != OGS_OK) return rv;

    rv = ogs_pfcp_context_parse_config(APP_NAME, "upf");
    if (rv != OGS_OK) return rv;

    rv = smf_context_parse_config();
    if (rv != OGS_OK) return rv;

    return ogs_pfcp_ue_pool_generate();
}

bool smf_part_uses_db(void)
{
    return smf_self()->persistence.enabled;
}

int smf_part_restore(void)
{
    /* PANIC: persistent context. Rebuild the sessions before the PFCP
     * association (their N4 sessions are re-created when it comes up) */
    return smf_persistence_restore();
}

void smf_part_register_eps(void)
{
    /* dumper /pdu-info */
    ogs_metrics_register_custom_ep(smf_dump_pdu_info, "/pdu-info");
}

int smf_part_open(void)
{
    int rv;

    rv = smf_fd_init();
    if (rv != 0) return OGS_ERROR;

    rv = smf_gtp_open();
    if (rv != 0) return OGS_ERROR;

    rv = smf_pfcp_open();
    if (rv != 0) return OGS_ERROR;

    return OGS_OK;
}

/*
 * Inside a box, only what the box's other NFs do not subscribe to already. The
 * PCF allows only the SMF on this service, so the subscription must say SMF.
 */
void smf_part_subscribe(void)
{
    ogs_sbi_subscription_spec_t *subscription_spec = NULL;

    subscription_spec = ogs_sbi_subscription_spec_add(
            OpenAPI_nf_type_NULL, OpenAPI_service_name_npcf_smpolicycontrol);
    ogs_assert(subscription_spec);
    subscription_spec->requester_nf_type = OpenAPI_nf_type_SMF;
}

void smf_part_stop(void)
{
    /* PANIC: persistent context. Shutdown removes the sessions from RAM but
     * does not release them: leave the database alone from here on */
    smf_persistence_stop();
}

void smf_part_close(void)
{
    smf_gtp_close();
    smf_pfcp_close();
}

void smf_part_final(void)
{
    smf_fd_final();

    smf_context_final();

    ogs_pfcp_context_final();
    ogs_gtp_context_final();

    ogs_pfcp_xact_final();
    ogs_gtp_xact_final();
}

int smf_part_event_base(void)
{
    return SMF_EVT_BASE;
}

int smf_part_event_top(void)
{
    return SMF_EVT_TOP;
}

static ogs_fsm_t part_sm;

void smf_part_fsm_init(void)
{
    ogs_fsm_init(&part_sm, smf_state_initial, smf_state_final, 0);
}

void smf_part_fsm_fini(void)
{
    ogs_fsm_fini(&part_sm, 0);
}

void smf_part_dispatch(void *e)
{
    ogs_fsm_dispatch(&part_sm, e);
}

static ogs_timer_t *t_termination_holding = NULL;

static void event_termination(void)
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
#define TERMINATION_HOLDING_TIME ogs_time_from_msec(300)
    ogs_timer_start(t_termination_holding, TERMINATION_HOLDING_TIME);

    /* Sending termination event to the queue */
    ogs_queue_term(ogs_app()->queue);
    ogs_pollset_notify(ogs_app()->pollset);
}

void smf_terminate(void)
{
    if (!initialized) return;

    smf_part_stop();

    /* Daemon terminating */
    event_termination();
    ogs_thread_destroy(thread);
    ogs_timer_delete(t_termination_holding);

    smf_part_close();
    smf_sbi_close();

    ogs_metrics_context_close(ogs_metrics_self());

    if (smf_part_uses_db())
        ogs_dbi_final();

    smf_part_final();
    ogs_sbi_context_final();

    smf_metrics_final();
}

static void smf_main(void *data)
{
    ogs_fsm_t smf_sm;
    int rv;

    ogs_fsm_init(&smf_sm, smf_state_initial, smf_state_final, 0);

    for ( ;; ) {
        ogs_pollset_poll(ogs_app()->pollset,
                ogs_timer_mgr_next(ogs_app()->timer_mgr));

        /*
         * After ogs_pollset_poll(), ogs_timer_mgr_expire() must be called.
         *
         * The reason is why ogs_timer_mgr_next() can get the current value
         * when ogs_timer_stop() is called internally in ogs_timer_mgr_expire().
         *
         * You should not use event-queue before ogs_timer_mgr_expire().
         * In this case, ogs_timer_mgr_expire() does not work
         * because 'if rv == OGS_DONE' statement is exiting and
         * not calling ogs_timer_mgr_expire().
         */
        ogs_timer_mgr_expire(ogs_app()->timer_mgr);

        for ( ;; ) {
            smf_event_t *e = NULL;

            rv = ogs_queue_trypop(ogs_app()->queue, (void**)&e);
            ogs_assert(rv != OGS_ERROR);

            if (rv == OGS_DONE)
                goto done;

            if (rv == OGS_RETRY)
                break;

            ogs_assert(e);
            ogs_fsm_dispatch(&smf_sm, e);
            ogs_event_free(e);
        }
    }
done:

    ogs_fsm_fini(&smf_sm, 0);
}
