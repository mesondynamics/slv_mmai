#include "ethernet_phy_policy.h"

bool EthernetPhy_IdentityIsValid(uint32_t identifier1,
                                 uint32_t identifier2)
{
  return (identifier1 == ECU_ETH_PHY_ID1_EXPECTED) &&
         ((identifier2 & ECU_ETH_PHY_ID2_MASK) ==
          ECU_ETH_PHY_ID2_EXPECTED);
}
