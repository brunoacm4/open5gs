/*
 * PANIC: the steps of amf_initialize() and amf_terminate(), so that a box
 * (src/box) can run the AMF next to other NFs in one process. A box sees only
 * this header, because each NF has its own context.h and event.h.
 */

#ifndef AMF_PART_H
#define AMF_PART_H

#include "ogs-core.h"

#ifdef __cplusplus
extern "C" {
#endif

void amf_part_metrics_init(void);
void amf_part_metrics_final(void);
void amf_part_context_init(void);
int amf_part_config(void);
bool amf_part_uses_db(void);
int amf_part_restore(void);
int amf_part_open(void);
void amf_part_stop(void);
void amf_part_close(void);
void amf_part_final(void);

int amf_part_event_base(void);
int amf_part_event_top(void);
void amf_part_fsm_init(void);
void amf_part_fsm_fini(void);
void amf_part_dispatch(void *e);

#ifdef __cplusplus
}
#endif

#endif /* AMF_PART_H */
