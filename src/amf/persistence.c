/*
 * PANIC: contexto persistente do AMF (fase 1b,
 * notes/core/fase1b-contexto-persistente.md).
 *
 * Coleção "amf_contexts", um documento por SUPI. Escreve-se quando a parte
 * estável muda: GUTI confirmado (registo, paging), sessão criada ou removida,
 * e o contador NAS de subida de 128 em 128 (para a primeira mensagem depois
 * de um reinício ser verificável: o AMF só reconstrói o contador se tiver
 * avançado menos de 256). Apaga-se no desregisto. Lê-se só no arranque.
 *
 * Não se guardam TEIDs, SEIDs, estado de ligação, temporizadores, nem as
 * referências a associações no PCF/UDM (o AMF restaurado não as tem).
 * As estruturas NAS de tamanho fixo vão em binário: o formato é interno.
 */

#include "persistence.h"
#include "amf-sm.h"

#define AMF_PERSISTENCE_COLLECTION "amf_contexts"
#define AMF_PERSISTENCE_UL_COUNT_STEP 128

/* Ligado depois do restauro e desligado antes de terminar (o encerramento
 * remove os contextos da RAM, não os desregista) */
static bool active = false;

static mongoc_collection_t *collection(void)
{
    ogs_mongoc_t *mongoc = ogs_mongoc();

    if (!mongoc || !mongoc->database)
        return NULL;
    return mongoc_database_get_collection(
            mongoc->database, AMF_PERSISTENCE_COLLECTION);
}

static void append_binary(bson_t *doc, const char *key,
        const void *data, size_t len)
{
    BSON_APPEND_BINARY(doc, key, BSON_SUBTYPE_BINARY,
            (const uint8_t *)data, (uint32_t)len);
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

void amf_persistence_save(amf_ue_t *amf_ue)
{
    mongoc_collection_t *coll = NULL;
    bson_t *doc = NULL, *selector = NULL, *opts = NULL;
    bson_t array, item, sub, subitem;
    bson_error_t error;
    amf_sess_t *sess = NULL;
    char key[16], subkey[16];
    int i, j, n;

    if (!active)
        return;
    ogs_assert(amf_ue);
    if (!amf_ue->supi || !amf_ue->current.m_tmsi ||
            !SECURITY_CONTEXT_IS_VALID(amf_ue))
        return;

    coll = collection();
    if (!coll)
        return;

    doc = bson_new();
    BSON_APPEND_UTF8(doc, "supi", amf_ue->supi);
    /* O AMF trata um UE sem SUCI como desconhecido (AMF_UE_HAVE_SUCI) */
    if (amf_ue->suci)
        BSON_APPEND_UTF8(doc, "suci", amf_ue->suci);
    BSON_APPEND_INT64(doc, "m_tmsi", *amf_ue->current.m_tmsi);

    BSON_APPEND_INT32(doc, "ue_tsc", amf_ue->nas.ue.tsc);
    BSON_APPEND_INT32(doc, "ue_ksi", amf_ue->nas.ue.ksi);
    BSON_APPEND_INT32(doc, "amf_tsc", amf_ue->nas.amf.tsc);
    BSON_APPEND_INT32(doc, "amf_ksi", amf_ue->nas.amf.ksi);
    append_binary(doc, "kamf", amf_ue->kamf, sizeof(amf_ue->kamf));
    BSON_APPEND_INT32(doc, "int_alg", amf_ue->selected_int_algorithm);
    BSON_APPEND_INT32(doc, "enc_alg", amf_ue->selected_enc_algorithm);
    append_binary(doc, "ue_sec_cap", &amf_ue->ue_security_capability,
            sizeof(amf_ue->ue_security_capability));
    BSON_APPEND_INT64(doc, "ul_count", amf_ue->ul_count.i32);
    BSON_APPEND_INT64(doc, "dl_count", amf_ue->dl_count);

    append_binary(doc, "allowed_nssai", &amf_ue->allowed_nssai,
            sizeof(amf_ue->allowed_nssai));
    append_binary(doc, "nr_tai", &amf_ue->nr_tai, sizeof(amf_ue->nr_tai));
    append_binary(doc, "nr_cgi", &amf_ue->nr_cgi, sizeof(amf_ue->nr_cgi));
    BSON_APPEND_INT64(doc, "ambr_ul", amf_ue->ue_ambr.uplink);
    BSON_APPEND_INT64(doc, "ambr_dl", amf_ue->ue_ambr.downlink);
    BSON_APPEND_INT64(doc, "t3512", amf_ue->t3512_value);

    /* Slices e DNNs da subscrição (para sessões novas depois do restauro) */
    BSON_APPEND_ARRAY_BEGIN(doc, "slices", &array);
    for (i = 0; i < amf_ue->num_of_slice; i++) {
        ogs_slice_data_t *slice = &amf_ue->slice[i];

        snprintf(key, sizeof(key), "%d", i);
        BSON_APPEND_DOCUMENT_BEGIN(&array, key, &item);
        BSON_APPEND_INT32(&item, "sst", slice->s_nssai.sst);
        BSON_APPEND_INT64(&item, "sd", slice->s_nssai.sd.v);
        BSON_APPEND_BOOL(&item, "default", slice->default_indicator);
        BSON_APPEND_ARRAY_BEGIN(&item, "dnn", &sub);
        for (j = 0; j < slice->num_of_session; j++) {
            if (!slice->session[j].name)
                continue;
            snprintf(subkey, sizeof(subkey), "%d", j);
            BSON_APPEND_DOCUMENT_BEGIN(&sub, subkey, &subitem);
            BSON_APPEND_UTF8(&subitem, "name", slice->session[j].name);
            BSON_APPEND_BOOL(&subitem, "lbo",
                    slice->session[j].lbo_roaming_allowed);
            bson_append_document_end(&sub, &subitem);
        }
        bson_append_array_end(&item, &sub);
        bson_append_document_end(&array, &item);
    }
    bson_append_array_end(doc, &array);

    /* Sessões com contexto no SMF */
    BSON_APPEND_ARRAY_BEGIN(doc, "sessions", &array);
    n = 0;
    ogs_list_for_each(&amf_ue->sess_list, sess) {
        if (!SESSION_CONTEXT_IN_SMF(sess) || !sess->sm_context_resource_uri)
            continue;
        snprintf(key, sizeof(key), "%d", n++);
        BSON_APPEND_DOCUMENT_BEGIN(&array, key, &item);
        BSON_APPEND_INT32(&item, "psi", sess->psi);
        BSON_APPEND_INT32(&item, "sst", sess->s_nssai.sst);
        BSON_APPEND_INT64(&item, "sd", sess->s_nssai.sd.v);
        if (sess->dnn)
            BSON_APPEND_UTF8(&item, "dnn", sess->dnn);
        BSON_APPEND_UTF8(&item, "sm_context_ref", sess->sm_context_ref);
        BSON_APPEND_UTF8(&item, "sm_context_uri",
                sess->sm_context_resource_uri);
        bson_append_document_end(&array, &item);
    }
    bson_append_array_end(doc, &array);

    selector = BCON_NEW("supi", BCON_UTF8(amf_ue->supi));
    opts = BCON_NEW("upsert", BCON_BOOL(true));
    if (!mongoc_collection_replace_one(
                coll, selector, doc, opts, NULL, &error))
        ogs_error("[%s] persistence: %s", amf_ue->supi, error.message);
    else
        amf_ue->persisted_ul_count = amf_ue->ul_count.i32;

    bson_destroy(opts);
    bson_destroy(selector);
    bson_destroy(doc);
    mongoc_collection_destroy(coll);
}

void amf_persistence_save_if_registered(amf_ue_t *amf_ue)
{
    if (!active)
        return;
    ogs_assert(amf_ue);
    if (OGS_FSM_CHECK(&amf_ue->sm, gmm_state_registered))
        amf_persistence_save(amf_ue);
}

void amf_persistence_save_ul_count(amf_ue_t *amf_ue)
{
    mongoc_collection_t *coll = NULL;
    bson_t *selector = NULL, *update = NULL;
    bson_error_t error;

    if (!active)
        return;
    ogs_assert(amf_ue);
    if (!amf_ue->supi)
        return;
    if ((uint32_t)(amf_ue->ul_count.i32 - amf_ue->persisted_ul_count) <
            AMF_PERSISTENCE_UL_COUNT_STEP)
        return;

    coll = collection();
    if (!coll)
        return;

    selector = BCON_NEW("supi", BCON_UTF8(amf_ue->supi));
    update = BCON_NEW("$set", "{",
            "ul_count", BCON_INT64(amf_ue->ul_count.i32), "}");
    if (!mongoc_collection_update_one(
                coll, selector, update, NULL, NULL, &error))
        ogs_error("[%s] persistence: %s", amf_ue->supi, error.message);
    else
        amf_ue->persisted_ul_count = amf_ue->ul_count.i32;

    bson_destroy(update);
    bson_destroy(selector);
    mongoc_collection_destroy(coll);
}

void amf_persistence_delete(amf_ue_t *amf_ue)
{
    mongoc_collection_t *coll = NULL;
    bson_t *selector = NULL;
    bson_error_t error;

    if (!active)
        return;
    ogs_assert(amf_ue);
    if (!amf_ue->supi)
        return;

    coll = collection();
    if (!coll)
        return;

    selector = BCON_NEW("supi", BCON_UTF8(amf_ue->supi));
    if (!mongoc_collection_delete_one(coll, selector, NULL, NULL, &error))
        ogs_error("[%s] persistence: %s", amf_ue->supi, error.message);

    bson_destroy(selector);
    mongoc_collection_destroy(coll);
}

void amf_persistence_stop(void)
{
    active = false;
}

/* Cliente SBI para o SMF da sessão, a partir do URI guardado (como em
 * amf_nsmf_pdusession_handle_create_sm_context) */
static void restore_sm_context_client(amf_sess_t *sess, const char *uri)
{
    bool rc;
    OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;
    char *fqdn = NULL;
    uint16_t fqdn_port = 0;
    ogs_sockaddr_t *addr = NULL, *addr6 = NULL;
    ogs_sbi_client_t *client = NULL;

    rc = ogs_sbi_getaddr_from_uri(
            &scheme, &fqdn, &fqdn_port, &addr, &addr6, (char *)uri);
    if (rc == false || scheme == OpenAPI_uri_scheme_NULL) {
        ogs_error("persistence: invalid URI [%s]", uri);
        return;
    }

    client = ogs_sbi_client_find(scheme, fqdn, fqdn_port, addr, addr6);
    if (!client)
        client = ogs_sbi_client_add(scheme, fqdn, fqdn_port, addr, addr6);
    if (client)
        OGS_SBI_SETUP_CLIENT(&sess->sm_context, client);
    else
        ogs_error("persistence: ogs_sbi_client_add() failed");

    ogs_free(fqdn);
    ogs_freeaddrinfo(addr);
    ogs_freeaddrinfo(addr6);
}

static void restore_slices(amf_ue_t *amf_ue, bson_iter_t *array)
{
    bson_iter_t item, sub, subitem;

    amf_ue->num_of_slice = 0;
    while (bson_iter_next(array) && amf_ue->num_of_slice < OGS_MAX_NUM_OF_SLICE) {
        ogs_slice_data_t *slice = &amf_ue->slice[amf_ue->num_of_slice];

        if (!BSON_ITER_HOLDS_DOCUMENT(array) ||
                !bson_iter_recurse(array, &item))
            continue;
        memset(slice, 0, sizeof(*slice));
        slice->s_nssai.sd.v = OGS_S_NSSAI_NO_SD_VALUE;

        while (bson_iter_next(&item)) {
            const char *k = bson_iter_key(&item);

            if (!strcmp(k, "sst"))
                slice->s_nssai.sst = bson_iter_as_int64(&item);
            else if (!strcmp(k, "sd"))
                slice->s_nssai.sd.v = bson_iter_as_int64(&item);
            else if (!strcmp(k, "default"))
                slice->default_indicator = bson_iter_as_bool(&item);
            else if (!strcmp(k, "dnn") && BSON_ITER_HOLDS_ARRAY(&item) &&
                    bson_iter_recurse(&item, &sub)) {
                while (bson_iter_next(&sub) &&
                        slice->num_of_session < OGS_MAX_NUM_OF_SESS) {
                    ogs_session_t *s = &slice->session[slice->num_of_session];

                    if (!bson_iter_recurse(&sub, &subitem))
                        continue;
                    while (bson_iter_next(&subitem)) {
                        const char *sk = bson_iter_key(&subitem);
                        if (!strcmp(sk, "name") &&
                                BSON_ITER_HOLDS_UTF8(&subitem))
                            s->name = ogs_strdup(
                                    bson_iter_utf8(&subitem, NULL));
                        else if (!strcmp(sk, "lbo"))
                            s->lbo_roaming_allowed =
                                bson_iter_as_bool(&subitem);
                    }
                    if (s->name)
                        slice->num_of_session++;
                }
            }
        }
        amf_ue->num_of_slice++;
    }
}

static void restore_sessions(amf_ue_t *amf_ue, bson_iter_t *array)
{
    bson_iter_t item;

    while (bson_iter_next(array)) {
        amf_sess_t *sess = NULL;
        int psi = 0;
        uint8_t sst = 0;
        uint32_t sd = OGS_S_NSSAI_NO_SD_VALUE;
        const char *dnn = NULL, *ref = NULL, *uri = NULL;

        if (!BSON_ITER_HOLDS_DOCUMENT(array) ||
                !bson_iter_recurse(array, &item))
            continue;
        while (bson_iter_next(&item)) {
            const char *k = bson_iter_key(&item);

            if (!strcmp(k, "psi"))
                psi = bson_iter_as_int64(&item);
            else if (!strcmp(k, "sst"))
                sst = bson_iter_as_int64(&item);
            else if (!strcmp(k, "sd"))
                sd = bson_iter_as_int64(&item);
            else if (!strcmp(k, "dnn") && BSON_ITER_HOLDS_UTF8(&item))
                dnn = bson_iter_utf8(&item, NULL);
            else if (!strcmp(k, "sm_context_ref") &&
                    BSON_ITER_HOLDS_UTF8(&item))
                ref = bson_iter_utf8(&item, NULL);
            else if (!strcmp(k, "sm_context_uri") &&
                    BSON_ITER_HOLDS_UTF8(&item))
                uri = bson_iter_utf8(&item, NULL);
        }
        if (psi == OGS_NAS_PDU_SESSION_IDENTITY_UNASSIGNED || !ref || !uri)
            continue;

        sess = amf_sess_add(amf_ue, psi);
        ogs_assert(sess);
        sess->s_nssai.sst = sst;
        sess->s_nssai.sd.v = sd;
        if (dnn)
            sess->dnn = ogs_strdup(dnn);
        STORE_SESSION_CONTEXT(sess, (char *)uri, (char *)ref);
        restore_sm_context_client(sess, uri);
    }
}

/* Um documento -> um UE registado em CM-IDLE. Devolve false se não deu. */
static bool restore_one(const bson_t *doc)
{
    bson_iter_t iter, array;
    amf_ue_t *amf_ue = NULL;
    amf_event_t e;
    const char *supi = NULL, *suci = NULL;
    uint32_t m_tmsi = 0;
    bool ok = true;

    if (bson_iter_init_find(&iter, doc, "supi") &&
            BSON_ITER_HOLDS_UTF8(&iter))
        supi = bson_iter_utf8(&iter, NULL);
    if (bson_iter_init_find(&iter, doc, "suci") &&
            BSON_ITER_HOLDS_UTF8(&iter))
        suci = bson_iter_utf8(&iter, NULL);
    if (bson_iter_init_find(&iter, doc, "m_tmsi"))
        m_tmsi = (uint32_t)bson_iter_as_int64(&iter);
    if (!supi || !suci || !m_tmsi) {
        ogs_error("persistence: record without SUPI, SUCI or M-TMSI");
        return false;
    }

    amf_ue = amf_ue_add(NULL);
    if (!amf_ue)
        return false;
    amf_ue_set_supi(amf_ue, (char *)supi);
    amf_ue->suci = ogs_strdup(suci);
    ogs_assert(amf_ue->suci);
    ogs_hash_set(amf_self()->suci_hash,
            amf_ue->suci, strlen(amf_ue->suci), amf_ue);

    /* GUTI: o mesmo M-TMSI, reservado no pool para não ser dado a outro UE */
    amf_ue->current.m_tmsi = amf_m_tmsi_reserve(m_tmsi);
    if (!amf_ue->current.m_tmsi) {
        ogs_error("[%s] persistence: M-TMSI 0x%x not available",
                supi, m_tmsi);
        amf_ue_remove(amf_ue);
        return false;
    }
    memset(&amf_ue->current.guti, 0, sizeof(ogs_nas_5gs_guti_t));
    ogs_nas_from_plmn_id(&amf_ue->current.guti.nas_plmn_id,
            &amf_ue->guami->plmn_id);
    memcpy(&amf_ue->current.guti.amf_id,
            &amf_ue->guami->amf_id, sizeof(ogs_amf_id_t));
    amf_ue->current.guti.m_tmsi = *amf_ue->current.m_tmsi;
    ogs_hash_set(amf_self()->guti_ue_hash, &amf_ue->current.guti,
            sizeof(ogs_nas_5gs_guti_t), amf_ue);

    if (!bson_iter_init(&iter, doc))
        ok = false;
    while (ok && bson_iter_next(&iter)) {
        const char *k = bson_iter_key(&iter);

        if (!strcmp(k, "ue_tsc"))
            amf_ue->nas.ue.tsc = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "ue_ksi"))
            amf_ue->nas.ue.ksi = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "amf_tsc"))
            amf_ue->nas.amf.tsc = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "amf_ksi"))
            amf_ue->nas.amf.ksi = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "kamf"))
            ok = get_binary(&iter, amf_ue->kamf, sizeof(amf_ue->kamf));
        else if (!strcmp(k, "int_alg"))
            amf_ue->selected_int_algorithm = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "enc_alg"))
            amf_ue->selected_enc_algorithm = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "ue_sec_cap"))
            ok = get_binary(&iter, &amf_ue->ue_security_capability,
                    sizeof(amf_ue->ue_security_capability));
        else if (!strcmp(k, "ul_count"))
            amf_ue->ul_count.i32 = (uint32_t)bson_iter_as_int64(&iter);
        else if (!strcmp(k, "dl_count"))
            amf_ue->dl_count = (uint32_t)bson_iter_as_int64(&iter);
        else if (!strcmp(k, "allowed_nssai"))
            ok = get_binary(&iter, &amf_ue->allowed_nssai,
                    sizeof(amf_ue->allowed_nssai));
        else if (!strcmp(k, "nr_tai"))
            ok = get_binary(&iter, &amf_ue->nr_tai, sizeof(amf_ue->nr_tai));
        else if (!strcmp(k, "nr_cgi"))
            ok = get_binary(&iter, &amf_ue->nr_cgi, sizeof(amf_ue->nr_cgi));
        else if (!strcmp(k, "ambr_ul"))
            amf_ue->ue_ambr.uplink = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "ambr_dl"))
            amf_ue->ue_ambr.downlink = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "t3512"))
            amf_ue->t3512_value = bson_iter_as_int64(&iter);
        else if (!strcmp(k, "slices") && BSON_ITER_HOLDS_ARRAY(&iter) &&
                bson_iter_recurse(&iter, &array))
            restore_slices(amf_ue, &array);
        else if (!strcmp(k, "sessions") && BSON_ITER_HOLDS_ARRAY(&iter) &&
                bson_iter_recurse(&iter, &array))
            restore_sessions(amf_ue, &array);
    }
    if (!ok || amf_ue->selected_int_algorithm == 0) {
        ogs_error("[%s] persistence: invalid record", supi);
        amf_ue_remove(amf_ue);
        return false;
    }

    /* Chaves NAS a partir do K_AMF guardado (como no Security Mode Command) */
    ogs_kdf_nas_5gs(OGS_KDF_NAS_INT_ALG, amf_ue->selected_int_algorithm,
            amf_ue->kamf, amf_ue->knas_int);
    ogs_kdf_nas_5gs(OGS_KDF_NAS_ENC_ALG, amf_ue->selected_enc_algorithm,
            amf_ue->kamf, amf_ue->knas_enc);
    amf_ue->security_context_available = 1;
    amf_ue->mac_failed = 0;
    amf_ue->persisted_ul_count = amf_ue->ul_count.i32;

    /* Na primeira mensagem, chave nova (fase 1b, patch 0012) */
    amf_ue->restored = true;

    /* Registado e em CM-IDLE: o mobile reachable timer corre desde já */
    memset(&e, 0, sizeof(e));
    e.amf_ue_id = amf_ue->id;
    ogs_fsm_tran(&amf_ue->sm, gmm_state_registered, &e);
    ogs_timer_start(amf_ue->mobile_reachable.timer,
            ogs_time_from_sec(amf_ue->t3512_value + 240));

    return true;
}

int amf_persistence_restore(void)
{
    mongoc_collection_t *coll = NULL;
    mongoc_cursor_t *cursor = NULL;
    const bson_t *doc = NULL;
    bson_t *query = NULL;
    bson_error_t error;
    int restored = 0, failed = 0;

    if (!amf_self()->persistence.enabled)
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

    ogs_info("[persistence] %d UE contexts restored (%d failed)",
            restored, failed);

    active = true;
    return OGS_OK;
}
