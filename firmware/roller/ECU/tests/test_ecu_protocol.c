#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ecu_protocol.h"

int main(void)
{
  ECU_ControlDatagramV2 source = {0};
  ECU_ControlDatagramV2 decoded = {0};
  ECU_V2Header header = {0};
  uint8_t frame[ECU_V2_HEADER_SIZE + sizeof(source)];
  static const uint8_t crc_vector[] = "123456789";
  size_t length;

  assert(sizeof(ECU_V2Header) == 24U);
  assert(sizeof(ECU_ControlDatagramV2) == 36U);
  assert(sizeof(ECU_StatusPayloadV2) == 100U);
  assert(sizeof(ECU_DiagnosticPayloadV2) == 86U);
  assert(sizeof(ECU_ValveConfigReplyPayload) == 72U);
  assert(ECU_ProtocolCrc32c(crc_vector, sizeof(crc_vector) - 1U) ==
         0xE3069283UL);

  source.sender_id = 2U;
  source.priority = 2U;
  source.control.pump_enable = 1U;
  source.control.valve_current_target_ma = -1370;
  source.control.engine_speed_level = 1;
  source.control.emergency_stop_request = 1U;

  length = ECU_ProtocolEncodeV2(
      ECU_MESSAGE_CONTROL_COMMAND, ECU_CONTROL_FLAG_CLEAR_FAULT,
      0x12345678UL, 0x76543210UL, &source, sizeof(source), frame,
      sizeof(frame));
  assert(length == sizeof(frame));
  assert(frame[0] == 0x45U && frame[1] == 0x43U &&
         frame[2] == 0x55U && frame[3] == 0x32U);
  assert(ECU_ProtocolDecodeV2(frame, length, &header, &decoded,
                              sizeof(decoded)));
  assert(header.message_type == ECU_MESSAGE_CONTROL_COMMAND);
  assert(header.flags == ECU_CONTROL_FLAG_CLEAR_FAULT);
  assert(header.sequence == 0x12345678UL);
  assert(header.timestamp_ms == 0x76543210UL);
  assert(memcmp(&source, &decoded, sizeof(source)) == 0);

  frame[ECU_V2_HEADER_SIZE + 3U] ^= 0x01U;
  assert(!ECU_ProtocolDecodeV2(frame, length, &header, &decoded,
                               sizeof(decoded)));
  frame[ECU_V2_HEADER_SIZE + 3U] ^= 0x01U;
  assert(!ECU_ProtocolDecodeV2(frame, length - 1U, &header, &decoded,
                               sizeof(decoded)));
  assert(!ECU_ProtocolDecodeV2(frame, length, &header, &decoded,
                               sizeof(decoded) - 1U));
  assert(ECU_ProtocolEncodeV2(ECU_MESSAGE_CONTROL_COMMAND, 0U, 1U, 1U,
                              &source, sizeof(source), frame,
                              sizeof(frame) - 1U) == 0U);
  return 0;
}
