/* Adapted from ST's NUCLEO-H563ZI LwIP_UDP_Echo_Server ethernetif port.
 * The CubeMX-generated heth/descriptors/TxConfig remain the single hardware
 * owner so future CubeMX regeneration cannot create duplicate ETH objects. */
#include "ethernetif.h"

#include <stddef.h>
#include <string.h>

#include "eth.h"
#include "ethernet_phy_policy.h"
#include "lan8742.h"
#include "lwip/memp.h"
#include "lwip/pbuf.h"
#include "netif/etharp.h"

#define ECU_ETH_IFNAME0                  'e'
#define ECU_ETH_IFNAME1                  '0'
#define ECU_ETH_RX_BUFFER_SIZE           1536U
#define ECU_ETH_RX_BUFFER_COUNT          12U
#define ECU_ETH_PHY_READ_ERROR_LIMIT        3U
#define ECU_ETH_RX_BUDGET       ETH_RX_DESC_CNT

extern ETH_TxPacketConfigTypeDef TxConfig;

typedef enum
{
  RX_ALLOC_OK = 0,
  RX_ALLOC_ERROR = 1
} RxAllocStatus;

typedef struct
{
  struct pbuf_custom custom;
  uint8_t buffer[ECU_ETH_RX_BUFFER_SIZE] __ALIGNED(32);
} RxBuffer;

LWIP_MEMPOOL_DECLARE(ECU_RX_POOL, ECU_ETH_RX_BUFFER_COUNT,
                     sizeof(RxBuffer), "ECU Ethernet RX pool");

static RxAllocStatus rx_alloc_status;
static lan8742_Object_t phy;
static EthernetPhyHealth phy_health;
static uint8_t phy_bus_registered;
static uint8_t phy_initialized_once;
static uint8_t phy_read_error_streak;

static int32_t PhyIoInit(void);
static int32_t PhyIoDeInit(void);
static int32_t PhyIoWrite(uint32_t device, uint32_t reg, uint32_t value);
static int32_t PhyIoRead(uint32_t device, uint32_t reg, uint32_t *value);
static int32_t PhyIoGetTick(void);
static bool EthernetPhyTryInitialize(void);
static void EthernetLinkSetDown(struct netif *netif);

static lan8742_IOCtx_t phy_io = {
  PhyIoInit, PhyIoDeInit, PhyIoWrite, PhyIoRead, PhyIoGetTick
};

static void RxBufferFree(struct pbuf *p)
{
  LWIP_MEMPOOL_FREE(ECU_RX_POOL, (struct pbuf_custom *)p);
  rx_alloc_status = RX_ALLOC_OK;
}

static err_t LowLevelOutput(struct netif *netif, struct pbuf *p)
{
  ETH_BufferTypeDef buffers[ETH_TX_DESC_CNT] = {0};
  struct pbuf *part;
  uint32_t count = 0U;
  HAL_StatusTypeDef status;

  if (!netif_is_up(netif) || !netif_is_link_up(netif))
  {
    return ERR_IF;
  }
  for (part = p; part != NULL; part = part->next)
  {
    if (count >= ETH_TX_DESC_CNT)
    {
      return ERR_IF;
    }
    buffers[count].buffer = part->payload;
    buffers[count].len = part->len;
    if (count != 0U)
    {
      buffers[count - 1U].next = &buffers[count];
    }
    ++count;
  }
  if (count == 0U)
  {
    return ERR_ARG;
  }

  /* Reclaim completed asynchronous descriptors before reserving another one.
     A pbuf reference is retained in pData until HAL_ETH_ReleaseTxPacket calls
     HAL_ETH_TxFreeCallback. This keeps the no-RTOS LwIP path non-blocking:
     status/diagnostic traffic can no longer stall RX polling for a HAL timeout. */
  (void)HAL_ETH_ReleaseTxPacket(&heth);
  TxConfig.Length = p->tot_len;
  TxConfig.TxBuffer = buffers;
  TxConfig.pData = p;
  pbuf_ref(p);
  status = HAL_ETH_Transmit_IT(&heth, &TxConfig);
  if (status != HAL_OK)
  {
    pbuf_free(p);
  }
  return (status == HAL_OK) ? ERR_OK : ERR_IF;
}

static struct pbuf *LowLevelInput(void)
{
  struct pbuf *packet = NULL;

  if (rx_alloc_status == RX_ALLOC_OK)
  {
    (void)HAL_ETH_ReadData(&heth, (void **)&packet);
  }
  return packet;
}

void ethernetif_input(struct netif *netif)
{
  struct pbuf *packet;
  uint32_t processed;

  /* Transmit completion is polled rather than interrupt-driven, but never
     waited for. The tight application loop therefore bounds RX latency while
     keeping descriptor/pbuf lifetime explicit. */
  (void)HAL_ETH_ReleaseTxPacket(&heth);
  /* Never let a continuous Ethernet stream monopolize the no-RTOS main loop.
     One pass drains at most the hardware RX ring; the next application pass
     promptly services safety, CAN and control timeouts before continuing. */
  for (processed = 0U; processed < ECU_ETH_RX_BUDGET; ++processed)
  {
    packet = LowLevelInput();
    if (packet == NULL)
    {
      break;
    }
    if (netif->input(packet, netif) != ERR_OK)
    {
      pbuf_free(packet);
    }
  }
}

err_t ethernetif_init(struct netif *netif)
{
  LWIP_ASSERT("netif", netif != NULL);
  netif->hostname = "roller-ecu";
  netif->name[0] = ECU_ETH_IFNAME0;
  netif->name[1] = ECU_ETH_IFNAME1;
  netif->output = etharp_output;
  netif->linkoutput = LowLevelOutput;
  netif->hwaddr_len = ETH_HWADDR_LEN;
  memcpy(netif->hwaddr, heth.Init.MACAddr, ETH_HWADDR_LEN);
  netif->mtu = ETH_MAX_PAYLOAD;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;

  LWIP_MEMPOOL_INIT(ECU_RX_POOL);
  rx_alloc_status = RX_ALLOC_OK;
  memset(&phy, 0, sizeof(phy));
  memset(&phy_health, 0, sizeof(phy_health));
  phy_bus_registered = 0U;
  phy_initialized_once = 0U;
  phy_read_error_streak = 0U;
  if (LAN8742_RegisterBusIO(&phy, &phy_io) != LAN8742_STATUS_OK)
  {
    return ERR_IF;
  }
  phy_bus_registered = 1U;
  if (!EthernetPhyTryInitialize())
  {
    /* PHY discovery is recoverable.  Keep the safety application and LwIP
       interface alive; the 250 ms link poll retries the fixed-address probe. */
    netif_set_link_down(netif);
    netif_set_down(netif);
    return ERR_OK;
  }
  ethernet_link_check_state(netif);
  return ERR_OK;
}

void ethernet_link_check_state(struct netif *netif)
{
  ETH_MACConfigTypeDef config = {0};
  int32_t state;
  uint32_t speed = 0U;
  uint32_t duplex = 0U;

  if ((phy.Is_Initialized == 0U) && !EthernetPhyTryInitialize())
  {
    EthernetLinkSetDown(netif);
    return;
  }

  state = LAN8742_GetLinkState(&phy);
  if (state < LAN8742_STATUS_OK)
  {
    ++phy_health.management_read_errors;
    phy_health.ready = 0U;
    if (phy_read_error_streak < UINT8_MAX)
    {
      ++phy_read_error_streak;
    }
    EthernetLinkSetDown(netif);
    if (phy_read_error_streak >= ECU_ETH_PHY_READ_ERROR_LIMIT)
    {
      (void)LAN8742_DeInit(&phy);
      phy_health.ready = 0U;
      phy_read_error_streak = 0U;
    }
    return;
  }
  phy_read_error_streak = 0U;
  phy_health.ready = 0U;

  if ((state == LAN8742_STATUS_LINK_DOWN) ||
      (state == LAN8742_STATUS_AUTONEGO_NOTDONE))
  {
    /* A valid down/not-yet-negotiated status proves the management path even
       without a cable, so it is sufficient for the OTA startup gate. */
    phy_health.ready = 1U;
    EthernetLinkSetDown(netif);
    return;
  }
  if (netif_is_link_up(netif))
  {
    phy_health.ready = 1U;
    return;
  }

  switch (state)
  {
    case LAN8742_STATUS_100MBITS_FULLDUPLEX:
      speed = ETH_SPEED_100M; duplex = ETH_FULLDUPLEX_MODE; break;
    case LAN8742_STATUS_100MBITS_HALFDUPLEX:
      speed = ETH_SPEED_100M; duplex = ETH_HALFDUPLEX_MODE; break;
    case LAN8742_STATUS_10MBITS_FULLDUPLEX:
      speed = ETH_SPEED_10M; duplex = ETH_FULLDUPLEX_MODE; break;
    case LAN8742_STATUS_10MBITS_HALFDUPLEX:
      speed = ETH_SPEED_10M; duplex = ETH_HALFDUPLEX_MODE; break;
    default:
      return;
  }

  (void)HAL_ETH_GetMACConfig(&heth, &config);
  config.Speed = speed;
  config.DuplexMode = duplex;
  if (HAL_ETH_SetMACConfig(&heth, &config) != HAL_OK)
  {
    ++phy_health.mac_state_errors;
    return;
  }
  if (HAL_ETH_Start(&heth) == HAL_OK)
  {
    netif_set_up(netif);
    netif_set_link_up(netif);
    /* With an attached cable, require the MAC data path to start before a
       test-swap image can be confirmed. */
    phy_health.ready = 1U;
  }
  else
  {
    ++phy_health.mac_state_errors;
  }
}

void ethernetif_get_phy_health(EthernetPhyHealth *health)
{
  if (health != NULL)
  {
    *health = phy_health;
  }
}

void HAL_ETH_RxAllocateCallback(uint8_t **buffer)
{
  struct pbuf_custom *custom = LWIP_MEMPOOL_ALLOC(ECU_RX_POOL);

  if (custom == NULL)
  {
    rx_alloc_status = RX_ALLOC_ERROR;
    *buffer = NULL;
    return;
  }
  *buffer = (uint8_t *)custom + offsetof(RxBuffer, buffer);
  custom->custom_free_function = RxBufferFree;
  (void)pbuf_alloced_custom(PBUF_RAW, 0U, PBUF_REF, custom, *buffer,
                            ECU_ETH_RX_BUFFER_SIZE);
}

void HAL_ETH_RxLinkCallback(void **start, void **end, uint8_t *buffer,
                            uint16_t length)
{
  struct pbuf **first = (struct pbuf **)start;
  struct pbuf **last = (struct pbuf **)end;
  struct pbuf *part = (struct pbuf *)(buffer - offsetof(RxBuffer, buffer));
  struct pbuf *cursor;

  part->next = NULL;
  part->len = length;
  part->tot_len = 0U;
  if (*first == NULL)
  {
    *first = part;
  }
  else
  {
    (*last)->next = part;
  }
  *last = part;
  for (cursor = *first; cursor != NULL; cursor = cursor->next)
  {
    cursor->tot_len = (u16_t)(cursor->tot_len + length);
  }
}

void HAL_ETH_TxFreeCallback(uint32_t *buffer)
{
  /* Asynchronous TX retains one pbuf reference in the descriptor until the
     polling release path observes DMA completion. */
  if (buffer != NULL)
  {
    pbuf_free((struct pbuf *)buffer);
  }
}

u32_t sys_now(void)
{
  return HAL_GetTick();
}

static int32_t PhyIoInit(void)
{
  HAL_ETH_SetMDIOClockRange(&heth);
  return 0;
}

static int32_t PhyIoDeInit(void)
{
  return 0;
}

static int32_t PhyIoWrite(uint32_t device, uint32_t reg, uint32_t value)
{
  if (!EthernetPhy_DeviceAddressIsAllowed(device))
  {
    /* The PCB straps PHYAD0 low. Reject the vendor driver's address scan in
       software so a stuck MDIO bus can consume at most one HAL timeout. */
    return -1;
  }
  return (HAL_ETH_WritePHYRegister(&heth, device, reg, value) == HAL_OK) ? 0 : -1;
}

static int32_t PhyIoRead(uint32_t device, uint32_t reg, uint32_t *value)
{
  if ((value == NULL) ||
      !EthernetPhy_DeviceAddressIsAllowed(device) ||
      (HAL_ETH_ReadPHYRegister(&heth, device, reg, value) != HAL_OK) ||
      ((*value & 0xFFFFU) == 0xFFFFU))
  {
    /* STM32 MDIO can report HAL_OK with all ones when no PHY responds.  No
       LAN8742 register consumed by this driver has 0xFFFF as a valid value. */
    return -1;
  }
  *value &= 0xFFFFU;
  return 0;
}

static int32_t PhyIoGetTick(void)
{
  return (int32_t)HAL_GetTick();
}

static bool EthernetPhyTryInitialize(void)
{
  uint32_t identifier1 = 0U;
  uint32_t identifier2 = 0U;

  if ((phy_bus_registered == 0U) ||
      (PhyIoRead(ECU_ETH_PHY_ADDRESS, LAN8742_PHYI1R, &identifier1) < 0) ||
      (PhyIoRead(ECU_ETH_PHY_ADDRESS, LAN8742_PHYI2R, &identifier2) < 0) ||
      !EthernetPhy_IdentityIsValid(identifier1, identifier2))
  {
    ++phy_health.initialization_failures;
    phy_health.ready = 0U;
    return false;
  }

  phy.Is_Initialized = 0U;
  if ((LAN8742_Init(&phy) != LAN8742_STATUS_OK) ||
      (phy.DevAddr != ECU_ETH_PHY_ADDRESS))
  {
    phy.Is_Initialized = 0U;
    ++phy_health.initialization_failures;
    phy_health.ready = 0U;
    return false;
  }

  if (phy_initialized_once != 0U)
  {
    ++phy_health.recoveries;
  }
  phy_initialized_once = 1U;
  phy_read_error_streak = 0U;
  /* ethernet_link_check_state() promotes this only after a complete BSR/BCR
     management read succeeds. */
  phy_health.ready = 0U;
  return true;
}

static void EthernetLinkSetDown(struct netif *netif)
{
  if (HAL_ETH_GetState(&heth) == HAL_ETH_STATE_STARTED)
  {
    if (HAL_ETH_Stop(&heth) != HAL_OK)
    {
      ++phy_health.mac_state_errors;
    }
  }
  if (netif_is_link_up(netif))
  {
    netif_set_link_down(netif);
  }
  if (netif_is_up(netif))
  {
    netif_set_down(netif);
  }
}
