#ifndef ECU_ETHERNETIF_H
#define ECU_ETHERNETIF_H

#include <stdint.h>

#include "lwip/err.h"
#include "lwip/netif.h"

typedef struct
{
  uint32_t initialization_failures;
  uint32_t management_read_errors;
  uint32_t recoveries;
  uint32_t mac_state_errors;
  uint8_t ready;
} EthernetPhyHealth;

err_t ethernetif_init(struct netif *netif);
void ethernetif_input(struct netif *netif);
void ethernet_link_check_state(struct netif *netif);
void ethernetif_get_phy_health(EthernetPhyHealth *health);

#endif /* ECU_ETHERNETIF_H */
