/*
 * PANIC: the steps of smf_initialize() and smf_terminate(), so that a box
 * (src/box) can run the SMF next to other NFs in one process. A box sees only
 * this header, because each NF has its own context.h and event.h.
 */

#ifndef SMF_PART_H
#define SMF_PART_H

#include "ogs-core.h"

#ifdef __cplusplus
extern "C" {
#endif

void smf_part_metrics_init(void);
void smf_part_metrics_final(void);
int smf_part_context_init(void);
void smf_part_instance_add(void);
int smf_part_config(void);
bool smf_part_uses_db(void);
int smf_part_restore(void);
void smf_part_register_eps(void);
int smf_part_open(void);
void smf_part_subscribe(void);
void smf_part_stop(void);
void smf_part_close(void);
void smf_part_final(void);

int smf_part_event_base(void);
int smf_part_event_top(void);
void smf_part_fsm_init(void);
void smf_part_fsm_fini(void);
void smf_part_dispatch(void *e);

#ifdef __cplusplus
}
#endif

#endif /* SMF_PART_H */
