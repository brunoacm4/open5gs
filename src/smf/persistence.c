/*
 * PANIC: persistent context of the SMF (phase 1b,
 * notes/core/fase1b-contexto-persistente.md).
 *
 * Collection "smf_sessions", one document per (SUPI, PSI), written when a 5GC
 * session becomes operational and deleted when it is removed. At start-up each
 * session is rebuilt in the same pool slot (its index is the sm_context_ref the
 * AMF kept) with the same UE IPv4 address, operational and deactivated; its
 * QoS flow and PFCP rules are built and sent to the UPF when the PFCP
 * association comes up (a plain Session Establishment: the UPF chooses new
 * TEIDs, which the UE never sees, and returns them in the Created PDRs).
 *
 * Not stored: TEIDs, SEIDs, PCF/BSF/UDM associations (the UDM keeps the SMF
 * registration in its database), the serving AMF instance id (NF instance ids
 * change on every start; with none, discovery finds the AMF). Only IPv4
 * sessions are restored.
 */

#include "persistence.h"
#include "npcf-handler.h"
#include "pfcp-path.h"

#define SMF_PERSISTENCE_COLLECTION "smf_sessions"

/* On after the restore and off before terminating (shutdown removes the
 * sessions from RAM, it does not release them) */
static bool active = false;

static mongoc_collection_t *collection(void)
{
    ogs_mongoc_t *mongoc = ogs_mongoc();

    if (!mongoc || !mongoc->database)
        return NULL;
    return mongoc_database_get_collection(
            mongoc->database, SMF_PERSISTENCE_COLLECTION);
}

static bool get_binary(const bson_iter_t *iter, void *data, size_t len)
{
    bson_subtype_t subtype;
    uint32_t got = 0;
    const uint8_t *bin = NULL;

    if (!BSON_ITER_HOLDS_BINARY(iter))
        return false;
    bson_iter_binary(iter, &subtype, &got, &bin);
    if (got != len)
        return false;
    memcpy(data, bin, len);
    return true;
}

void smf_persistence_save(smf_sess_t *sess)
{
    mongoc_collection_t *coll = NULL;
    bson_t *doc = NULL, *selector = NULL, *opts = NULL;
    bson_error_t error;
    smf_ue_t *smf_ue = NULL;

    if (!active)
        return;
    ogs_assert(sess);
    smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
    if (!smf_ue || !smf_ue->supi || sess->epc || !sess->session.name ||
            sess->session.session_type != OGS_PDU_SESSION_TYPE_IPV4 ||
            !sess->ipv4)
        return;

    coll = collection();
    if (!coll)
        return;

    doc = bson_new();
    BSON_APPEND_UTF8(doc, "supi", smf_ue->supi);
    BSON_APPEND_INT32(doc, "psi", sess->psi);
    BSON_APPEND_INT32(doc, "index", sess->index);
    BSON_APPEND_UTF8(doc, "dnn", sess->session.name);
    BSON_APPEND_INT32(doc, "sst", sess->s_nssai.sst);
    BSON_APPEND_INT64(doc, "sd", sess->s_nssai.sd.v);
    BSON_APPEND_INT32(doc, "session_type", sess->session.session_type);
    BSON_APPEND_INT32(doc, "ssc_mode", sess->session.ssc_mode);
    BSON_APPEND_BINARY(doc, "qos", BSON_SUBTYPE_BINARY,
            (const uint8_t *)&sess->session.qos, sizeof(sess->session.qos));
    BSON_APPEND_BINARY(doc, "ambr", BSON_SUBTYPE_BINARY,
            (const uint8_t *)&sess->session.ambr, sizeof(sess->session.ambr));
    BSON_APPEND_BINARY(doc, "ipv4", BSON_SUBTYPE_BINARY,
            (const uint8_t *)sess->ipv4->addr, OGS_IPV4_LEN);

    selector = BCON_NEW("supi", BCON_UTF8(smf_ue->supi),
            "psi", BCON_INT32(sess->psi));
    opts = BCON_NEW("upsert", BCON_BOOL(true));
    if (!mongoc_collection_replace_one(
                coll, selector, doc, opts, NULL, &error))
        ogs_error("[%s:%d] persistence: %s",
                smf_ue->supi, sess->psi, error.message);

    bson_destroy(opts);
    bson_destroy(selector);
    bson_destroy(doc);
    mongoc_collection_destroy(coll);
}

void smf_persistence_delete(smf_sess_t *sess)
{
    mongoc_collection_t *coll = NULL;
    bson_t *selector = NULL;
    bson_error_t error;
    smf_ue_t *smf_ue = NULL;

    if (!active)
        return;
    ogs_assert(sess);
    smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
    if (!smf_ue || !smf_ue->supi || sess->epc)
        return;

    coll = collection();
    if (!coll)
        return;

    selector = BCON_NEW("supi", BCON_UTF8(smf_ue->supi),
            "psi", BCON_INT32(sess->psi));
    if (!mongoc_collection_delete_one(coll, selector, NULL, NULL, &error))
        ogs_error("[%s:%d] persistence: %s",
                smf_ue->supi, sess->psi, error.message);

    bson_destroy(selector);
    mongoc_collection_destroy(coll);
}

void smf_persistence_stop(void)
{
    active = false;
}

/* One document -> one operational, deactivated session without N4 rules */
static bool restore_one(const bson_t *doc)
{
    bson_iter_t iter;
    smf_ue_t *smf_ue = NULL;
    smf_sess_t *sess = NULL;
    smf_event_t e;
    const char *supi = NULL, *dnn = NULL;
    int psi = 0, index = 0, session_type = 0, ssc_mode = 0;
    uint8_t sst = 0;
    uint32_t sd = OGS_S_NSSAI_NO_SD_VALUE;
    ogs_qos_t qos;
    ogs_bitrate_t ambr;
    uint8_t ipv4[OGS_IPV4_LEN];
    bool have_qos = false, have_ambr = false, have_ipv4 = false;

    memset(&qos, 0, sizeof(qos));
    memset(&ambr, 0, sizeof(ambr));

    if (!bson_iter_init(&iter, doc))
        return false;
    while (bson_iter_next(&iter)) {
        const char *k = bson_iter_key(&iter);

        if (!strcmp(k, "supi") && BSON_ITER_HOLDS_UTF8(&iter))
            supi = bson_iter_utf8(&iter, NULL);
        else if (!strcmp(k, "psi"))
            psi = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "index"))
            index = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "dnn") && BSON_ITER_HOLDS_UTF8(&iter))
            dnn = bson_iter_utf8(&iter, NULL);
        else if (!strcmp(k, "sst"))
            sst = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "sd"))
            sd = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "session_type"))
            session_type = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "ssc_mode"))
            ssc_mode = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "qos"))
            have_qos = get_binary(&iter, &qos, sizeof(qos));
        else if (!strcmp(k, "ambr"))
            have_ambr = get_binary(&iter, &ambr, sizeof(ambr));
        else if (!strcmp(k, "ipv4"))
            have_ipv4 = get_binary(&iter, ipv4, sizeof(ipv4));
    }
    if (!supi || !dnn || psi == OGS_NAS_PDU_SESSION_IDENTITY_UNASSIGNED ||
            index <= 0 || session_type != OGS_PDU_SESSION_TYPE_IPV4 ||
            !have_qos || !have_ambr || !have_ipv4) {
        ogs_error("persistence: invalid session record");
        return false;
    }

    smf_ue = smf_ue_find_by_supi((char *)supi);
    if (!smf_ue)
        smf_ue = smf_ue_add_by_supi((char *)supi);
    if (!smf_ue)
        return false;

    sess = smf_sess_add_restored(smf_ue, psi, index);
    if (!sess) {
        ogs_error("[%s:%d] persistence: index %d not available",
                supi, psi, index);
        if (ogs_list_count(&smf_ue->sess_list) == 0)
            smf_ue_remove(smf_ue);
        return false;
    }

    sess->session.name = ogs_strdup(dnn);
    ogs_assert(sess->session.name);
    sess->s_nssai.sst = sst;
    sess->s_nssai.sd.v = sd;
    sess->session.session_type = session_type;
    sess->session.ssc_mode = ssc_mode;
    memcpy(&sess->session.qos, &qos, sizeof(qos));
    memcpy(&sess->session.ambr, &ambr, sizeof(ambr));

    /* The same UE IPv4 address, taken from the dynamic pool */
    sess->ipv4 = ogs_pfcp_ue_ip_reserve(AF_INET, dnn, ipv4);
    if (!sess->ipv4) {
        ogs_error("[%s:%d] persistence: UE IPv4 address not available",
                supi, psi);
        smf_sess_remove(sess);
        if (ogs_list_count(&smf_ue->sess_list) == 0)
            smf_ue_remove(smf_ue);
        return false;
    }
    sess->paa.session_type = session_type;
    sess->paa.addr = sess->ipv4->addr[0];
    ogs_hash_set(smf_self()->ipv4_hash, sess->ipv4->addr, OGS_IPV4_LEN, sess);

    /* Established before the restart, now idle: the UDM still has the SMF
     * registration (patch 0004 skips a new one on activation) */
    sess->up_cnx_state = OpenAPI_up_cnx_state_DEACTIVATED;
    sess->udm_registered = true;
    sess->restored = true;

    memset(&e, 0, sizeof(e));
    e.sess_id = sess->id;
    ogs_fsm_tran(&sess->sm, smf_gsm_state_operational, &e);

    return true;
}

int smf_persistence_restore(void)
{
    mongoc_collection_t *coll = NULL;
    mongoc_cursor_t *cursor = NULL;
    const bson_t *doc = NULL;
    bson_t *query = NULL;
    bson_error_t error;
    int restored = 0, failed = 0;

    if (!smf_self()->persistence.enabled)
        return OGS_OK;

    coll = collection();
    if (!coll) {
        ogs_error("persistence: no database");
        return OGS_ERROR;
    }

    query = bson_new();
    cursor = mongoc_collection_find_with_opts(coll, query, NULL, NULL);
    while (mongoc_cursor_next(cursor, &doc)) {
        if (restore_one(doc))
            restored++;
        else
            failed++;
    }
    if (mongoc_cursor_error(cursor, &error))
        ogs_error("persistence: %s", error.message);

    mongoc_cursor_destroy(cursor);
    bson_destroy(query);
    mongoc_collection_destroy(coll);

    ogs_info("[persistence] %d PDU sessions restored (%d failed)",
            restored, failed);

    active = true;
    return OGS_OK;
}

/* PFCP association up: re-create the N4 session of each restored session */
void smf_persistence_reestablish(ogs_pfcp_node_t *node)
{
    smf_ue_t *smf_ue = NULL;
    smf_sess_t *sess = NULL;
    int n = 0;

    ogs_assert(node);

    ogs_list_for_each(&smf_self()->smf_ue_list, smf_ue) {
        ogs_list_for_each(&smf_ue->sess_list, sess) {
            if (!sess->restored)
                continue;
            if (!sess->pfcp_node)
                smf_sess_select_upf(sess);
            if (sess->pfcp_node != node)
                continue;

            /* Without the restoration flag: with it the UPF does not return
             * the Created PDRs, and the TEIDs it chose are needed here (the
             * old ones were not kept) */
            smf_sess_build_default_rules(sess);
            ogs_assert(OGS_OK ==
                smf_5gc_pfcp_send_session_establishment_request(
                    sess, NULL, 0));
            n++;
        }
    }

    if (n)
        ogs_info("[persistence] %d N4 sessions re-created", n);
}
