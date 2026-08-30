#ifndef ECU_ETHERNET_PHY_POLICY_H
#define ECU_ETHERNET_PHY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* PHYAD0 is strapped low on this ECU, so accepting any scanned address would
 * hide an open MDIO bus.  LAN8742A revision bits are intentionally ignored. */
#define ECU_ETH_PHY_ADDRESS             0U
#define ECU_ETH_PHY_ID1_EXPECTED   0x0007U
#define ECU_ETH_PHY_ID2_MASK       0xFFF0U
#define ECU_ETH_PHY_ID2_EXPECTED   0xC130U

bool EthernetPhy_IdentityIsValid(uint32_t identifier1,
                                 uint32_t identifier2);

#endif /* ECU_ETHERNET_PHY_POLICY_H */
