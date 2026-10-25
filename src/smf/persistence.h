/*
 * PANIC: persistent context of the SMF (phase 1b).
 *
 * The stable part of each 5GC PDU session (identity, index = sm_context_ref,
 * DNN, S-NSSAI, QoS, UE IPv4 address) is stored in the MongoDB of db_uri and
 * rebuilt at start-up; its N4 session is re-created in the UPF when the PFCP
 * association comes up. RAM stays the source of truth; the database is a
 * copy read only at start-up.
 */

#ifndef SMF_PERSISTENCE_H
#define SMF_PERSISTENCE_H

#include "context.h"

#ifdef __cplusplus
extern "C" {
#endif

int smf_persistence_restore(void);
void smf_persistence_reestablish(ogs_pfcp_node_t *node);
void smf_persistence_stop(void);

void smf_persistence_save(smf_sess_t *sess);
void smf_persistence_delete(smf_sess_t *sess);

#ifdef __cplusplus
}
#endif

#endif /* SMF_PERSISTENCE_H */
