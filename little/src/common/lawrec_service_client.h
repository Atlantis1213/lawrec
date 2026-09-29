#ifndef __LAWREC_SERVICE_CLIENT_H__
#define __LAWREC_SERVICE_CLIENT_H__

#include "lawrec_service_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

int lawrec_service_call(lawrec_service_cmd_e cmd, uint32_t value,
                        lawrec_service_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif
