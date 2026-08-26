#include "j1939.h"

#include <string.h>

#include "fdcan.h"
#include "main.h"

#define J1939_FILTER_MASK 0x03FFFF00UL

static J1939Data vehicle_data;

static uint8_t J1939_ClampPercent(int16_t value)
{
  if (value < 0) { return 0U; }
  if (value > 100) { return 100U; }
  return (uint8_t)value;
}

static uint32_t J1939_ExtractPgn(uint32_t identifier)
{
  uint8_t pf = (uint8_t)(identifier >> 16U);
  uint8_t ps = (uint8_t)(identifier >> 8U);
  return (pf < 240U) ? ((uint32_t)pf << 8U) :
                       (((uint32_t)pf << 8U) | ps);
}

static void J1939_Process(uint32_t pgn, const uint8_t data[8])
{
  uint32_t now = HAL_GetTick();

  switch (pgn)
  {
    case J1939_PGN_EEC1:
      if (data[2] <= 250U)
      {
        vehicle_data.driver_demand_percent =
            J1939_ClampPercent((int16_t)data[2] - 125);
      }
      if (data[3] <= 250U)
      {
        vehicle_data.engine_torque_percent =
            J1939_ClampPercent((int16_t)data[3] - 125);
      }
      {
        uint16_t raw = (uint16_t)data[4] | ((uint16_t)data[5] << 8U);
        if (raw <= 0xFAFFU) { vehicle_data.engine_rpm = raw; }
      }
      vehicle_data.eec1_valid = true;
      vehicle_data.eec1_timestamp = now;
      break;

    case J1939_PGN_EEC2:
      if (data[1] <= 250U)
      {
        vehicle_data.accelerator_pedal_percent =
            J1939_ClampPercent((int16_t)(((uint32_t)data[1] * 40U) / 100U));
      }
      if (data[2] <= 250U)
      {
        vehicle_data.engine_load_percent = J1939_ClampPercent(data[2]);
      }
      vehicle_data.eec2_valid = true;
      vehicle_data.eec2_timestamp = now;
      break;

    case J1939_PGN_ET1:
      if (data[0] <= 250U)
      {
        vehicle_data.coolant_temp_cdeg =
            (int16_t)(((int16_t)data[0] - 40) * 100);
      }
      if (data[1] <= 250U)
      {
        vehicle_data.fuel_temp_cdeg =
            (int16_t)(((int16_t)data[1] - 40) * 100);
      }
      vehicle_data.et1_valid = true;
      vehicle_data.et1_timestamp = now;
      break;

    case J1939_PGN_EFL_P1:
      if (data[3] <= 250U)
      {
        vehicle_data.oil_pressure_kpa = (uint16_t)data[3] * 4U;
      }
      if (data[2] <= 250U)
      {
        vehicle_data.fuel_pressure_kpa = (uint16_t)data[2] * 4U;
      }
      vehicle_data.eflp1_valid = true;
      vehicle_data.eflp1_timestamp = now;
      break;

    case J1939_PGN_AMB:
      {
        uint16_t raw = (uint16_t)data[3] | ((uint16_t)data[4] << 8U);
        if (raw <= 0xFAFFU)
        {
          int32_t cdeg = (((int32_t)raw * 3125L / 100000L) - 273L) * 100L;
          vehicle_data.ambient_temp_cdeg = (int16_t)cdeg;
        }
      }
      vehicle_data.amb_valid = true;
      vehicle_data.amb_timestamp = now;
      break;

    case J1939_PGN_DM1:
      /* Preserve the previous ECU's single-frame behavior. Transport
         protocol/multi-packet DTC decoding is intentionally not invented. */
      vehicle_data.dtc_count = 0U;
      vehicle_data.dm1_valid = true;
      vehicle_data.dm1_timestamp = now;
      break;
    default:
      break;
  }
}

bool J1939_Init(void)
{
  static const uint32_t pgns[6] = {
    J1939_PGN_EEC1, J1939_PGN_EEC2, J1939_PGN_ET1,
    J1939_PGN_EFL_P1, J1939_PGN_AMB, J1939_PGN_DM1
  };
  FDCAN_FilterTypeDef filter = {0};
  uint32_t index;

  memset(&vehicle_data, 0, sizeof(vehicle_data));
  for (index = 0U; index < 6U; ++index)
  {
    filter.IdType = FDCAN_EXTENDED_ID;
    filter.FilterIndex = index;
    filter.FilterType = FDCAN_FILTER_MASK;
    filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    filter.FilterID1 = pgns[index] << 8U;
    filter.FilterID2 = J1939_FILTER_MASK;
    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &filter) != HAL_OK)
    {
      return false;
    }
  }
  if ((HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_REJECT, FDCAN_REJECT,
                                    FDCAN_REJECT_REMOTE,
                                    FDCAN_REJECT_REMOTE) != HAL_OK) ||
      (HAL_FDCAN_ActivateNotification(&hfdcan1,
                                      FDCAN_IT_RX_FIFO0_NEW_MESSAGE,
                                      0U) != HAL_OK) ||
      (HAL_FDCAN_Start(&hfdcan1) != HAL_OK))
  {
    return false;
  }
  return true;
}

void J1939_Poll(void)
{
  uint32_t now = HAL_GetTick();
  uint32_t primask = __get_PRIMASK();

  /* The receive callback updates the same validity/timestamp pairs in an
     interrupt.  Keep expiry decisions atomic so an ISR cannot refresh a PGN
     between the stale check and clearing its valid flag. */
  __disable_irq();

  if (vehicle_data.eec1_valid &&
      ((now - vehicle_data.eec1_timestamp) > J1939_DATA_TIMEOUT_MS))
  { vehicle_data.eec1_valid = false; }
  if (vehicle_data.eec2_valid &&
      ((now - vehicle_data.eec2_timestamp) > J1939_DATA_TIMEOUT_MS))
  { vehicle_data.eec2_valid = false; }
  if (vehicle_data.et1_valid &&
      ((now - vehicle_data.et1_timestamp) > J1939_DATA_TIMEOUT_MS))
  { vehicle_data.et1_valid = false; }
  if (vehicle_data.eflp1_valid &&
      ((now - vehicle_data.eflp1_timestamp) > J1939_DATA_TIMEOUT_MS))
  { vehicle_data.eflp1_valid = false; }
  if (vehicle_data.amb_valid &&
      ((now - vehicle_data.amb_timestamp) > J1939_DATA_TIMEOUT_MS))
  { vehicle_data.amb_valid = false; }
  if (vehicle_data.dm1_valid &&
      ((now - vehicle_data.dm1_timestamp) > J1939_DATA_TIMEOUT_MS))
  { vehicle_data.dm1_valid = false; }
  if (primask == 0U) { __enable_irq(); }
}

void J1939_GetData(J1939Data *data)
{
  uint32_t primask;

  if (data == NULL) { return; }
  primask = __get_PRIMASK();
  __disable_irq();
  *data = vehicle_data;
  if (primask == 0U) { __enable_irq(); }
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan,
                               uint32_t fifo_interrupts)
{
  FDCAN_RxHeaderTypeDef header;
  uint8_t bytes[8];

  if ((hfdcan->Instance != FDCAN1) ||
      ((fifo_interrupts & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) == 0U))
  {
    return;
  }
  while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) != 0U)
  {
    if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &header, bytes) != HAL_OK)
    {
      break;
    }
    if ((header.IdType == FDCAN_EXTENDED_ID) &&
        (header.FDFormat == FDCAN_CLASSIC_CAN) &&
        (header.DataLength == FDCAN_DLC_BYTES_8))
    {
      J1939_Process(J1939_ExtractPgn(header.Identifier), bytes);
    }
  }
}
