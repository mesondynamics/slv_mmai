#include "ethernet_phy_policy.h"

#include <assert.h>

int main(void)
{
  uint32_t revision;

  for (revision = 0U; revision <= 0x0FU; ++revision)
  {
    assert(EthernetPhy_IdentityIsValid(0x0007U, 0xC130U | revision));
  }

  /* An open MDIO bus commonly reads all ones.  It must never be accepted as
     address 31, which the unqualified ST address scanner would otherwise do. */
  assert(!EthernetPhy_IdentityIsValid(0xFFFFU, 0xFFFFU));
  assert(!EthernetPhy_IdentityIsValid(0x0000U, 0x0000U));
  assert(!EthernetPhy_IdentityIsValid(0x0007U, 0xC120U));
  assert(!EthernetPhy_IdentityIsValid(0x0006U, 0xC130U));
  return 0;
}
