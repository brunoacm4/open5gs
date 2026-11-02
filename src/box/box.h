/*
 * PANIC: a box runs several NFs (its "parts") in one process, with one event
 * loop. Each part keeps its own state machine; the box decides whose each event
 * is. The first part of the table also owns what the parts share, which is the
 * NRF management of the process.
 */

#ifndef BOX_H
#define BOX_H

#include "ogs-sbi.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int __box_log_domain;

#undef OGS_LOG_DOMAIN
#define OGS_LOG_DOMAIN __box_log_domain

typedef struct box_part_s {
    OpenAPI_nf_type_e nf_type;  /* requester type of its SBI transactions */
    const char *uri_prefix;     /* SBI requests with this path prefix are its */
    int (*event_base)(void);    /* its private events: base < id < top */
    int (*event_top)(void);
    void (*fsm_init)(void);
    void (*fsm_fini)(void);
    void (*dispatch)(void *e);
} box_part_t;

int box_start(const box_part_t *parts, int num_of_parts);
void box_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* BOX_H */
