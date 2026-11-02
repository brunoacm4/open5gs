/*
 * PANIC: the "Acesso e Sessao" box, the AMF and the SMF in one process.
 *
 * One YAML with the sections global, amf and smf (notes/adr/0003): one SBI
 * server, one NRF client and one metrics server, all under amf. Only the AMF
 * registers in the NRF; the SMF is a local instance (notes/adr/0002).
 */

#include "ogs-sctp.h"
#include "ogs-app.h"
#include "ogs-dbi.h"
#include "ogs-metrics.h"

#include "box.h"
#include "../amf/part.h"
#include "../smf/part.h"

static const box_part_t parts[] = {
    { OpenAPI_nf_type_AMF, NULL,
      amf_part_event_base, amf_part_event_top,
      amf_part_fsm_init, amf_part_fsm_fini, amf_part_dispatch },
    { OpenAPI_nf_type_SMF, "/nsmf-",
      smf_part_event_base, smf_part_event_top,
      smf_part_fsm_init, smf_part_fsm_fini, smf_part_dispatch },
};

static int initialized = 0;

int app_initialize(const char *const argv[])
{
    int rv;

    ogs_log_install_domain(&__box_log_domain, "box", ogs_core()->log.level);

    ogs_sctp_init(ogs_app()->usrsctp.udp_port);

    rv = ogs_app_parse_local_conf("amf");
    if (rv != OGS_OK) return rv;

    ogs_metrics_context_init();
    amf_part_metrics_init();
    smf_part_metrics_init();

    ogs_sbi_context_init(OpenAPI_nf_type_AMF);

    amf_part_context_init();
    rv = smf_part_context_init();
    if (rv != OGS_OK) return rv;

    rv = ogs_log_config_domain(
            ogs_app()->logger.domain, ogs_app()->logger.level);
    if (rv != OGS_OK) return rv;

    rv = ogs_sbi_context_parse_config("amf", "nrf", "scp");
    if (rv != OGS_OK) return rv;

    rv = ogs_metrics_context_parse_config("amf");
    if (rv != OGS_OK) return rv;

    /* Before its config: smf.info goes to this instance, not to self */
    smf_part_instance_add();

    rv = amf_part_config();
    if (rv != OGS_OK) return rv;

    rv = smf_part_config();
    if (rv != OGS_OK) return rv;

    if (amf_part_uses_db() || smf_part_uses_db()) {
        if (!ogs_app()->db_uri) {
            ogs_error("persistence.enabled needs db_uri");
            return OGS_ERROR;
        }
        rv = ogs_dbi_init(ogs_app()->db_uri);
        if (rv != OGS_OK) return rv;
    }

    ogs_metrics_context_open(ogs_metrics_self());

    rv = amf_part_restore();
    if (rv != OGS_OK) return rv;

    rv = smf_part_restore();
    if (rv != OGS_OK) return rv;

    smf_part_register_eps();

    rv = smf_part_open();
    if (rv != OGS_OK) return rv;

    rv = amf_part_open();
    if (rv != OGS_OK) return rv;

    /* The SMF part reaches the AMF part through self, which has no client */
    ogs_sbi_client_associate(ogs_sbi_self()->nf_instance);

    smf_part_subscribe();

    rv = box_start(parts, OGS_ARRAY_SIZE(parts));
    if (rv != OGS_OK) return rv;

    initialized = 1;

    ogs_info("AMF+SMF box initialize...done");

    return OGS_OK;
}

void app_terminate(void)
{
    if (!initialized) return;

    amf_part_stop();
    smf_part_stop();

    box_stop();

    smf_part_close();
    amf_part_close();

    ogs_metrics_context_close(ogs_metrics_self());

    if (amf_part_uses_db() || smf_part_uses_db())
        ogs_dbi_final();

    amf_part_final();
    smf_part_final();
    ogs_sbi_context_final();

    smf_part_metrics_final();
    amf_part_metrics_final();
    ogs_metrics_context_final();

    ogs_sctp_final();

    ogs_info("AMF+SMF box terminate...done");
}
