#ifndef SECURITY_FACTORY_H
#define SECURITY_FACTORY_H

#include <stdint.h>

#include "safety_api.h"

#if defined(ECU_FACTORY_PROVISIONING)
int32_t SecurityFactory_GetStatus(SAFETY_FactoryStatus *status);
int32_t SecurityFactory_Provision(
    const SAFETY_FactoryProvisionRequest *request,
    SAFETY_FactoryStatus *status);
#endif

#endif /* SECURITY_FACTORY_H */
