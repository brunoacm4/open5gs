/*
 * PANIC: contexto persistente do AMF (fase 1b).
 *
 * A parte estável do contexto de cada UE registado (identidade, contexto de
 * segurança, slices, sessões) é guardada no MongoDB de db_uri e reconstruída
 * no arranque, para a frota voltar sem autenticação nem sessão nova depois de
 * um reinício do core. A RAM continua a ser a fonte de verdade; a base de
 * dados é uma cópia lida só no arranque.
 */

#ifndef AMF_PERSISTENCE_H
#define AMF_PERSISTENCE_H

#include "context.h"

#ifdef __cplusplus
extern "C" {
#endif

int amf_persistence_restore(void);
void amf_persistence_stop(void);

void amf_persistence_save(amf_ue_t *amf_ue);
void amf_persistence_save_if_registered(amf_ue_t *amf_ue);
void amf_persistence_save_ul_count(amf_ue_t *amf_ue);
void amf_persistence_delete(amf_ue_t *amf_ue);

#ifdef __cplusplus
}
#endif

#endif /* AMF_PERSISTENCE_H */
