#include "ecu_protocol.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(ECU_V2Header) == ECU_V2_HEADER_SIZE,
               "V2 frame header ABI changed");
_Static_assert(sizeof(ECU_ControlPayloadV2) == 32U,
               "V2 control payload ABI changed");
_Static_assert(sizeof(ECU_ControlDatagramV2) == 36U,
               "V2 control datagram ABI changed");
_Static_assert(sizeof(ECU_StatusPayloadV2) == 100U,
               "V2 status payload ABI changed");
_Static_assert(sizeof(ECU_DiagnosticPayloadV2) == 86U,
               "V2 diagnostic payload ABI changed");
_Static_assert(sizeof(ECU_TelemetrySubscribePayload) == 8U,
               "V2 subscription payload ABI changed");
_Static_assert(sizeof(SAFETY_ValveConfig) == 48U,
               "Valve configuration ABI changed");
_Static_assert(sizeof(SAFETY_ValveConfigSnapshot) == 64U,
               "Valve configuration snapshot ABI changed");
_Static_assert(sizeof(ECU_ValveConfigReplyPayload) == 72U,
               "V2 valve configuration reply ABI changed");
_Static_assert(sizeof(ECU_OperationAckPayload) == 8U,
               "V2 operation acknowledgement ABI changed");
_Static_assert(sizeof(SAFETY_ValveTelemetrySample) == 20U,
               "Valve telemetry sample ABI changed");

uint32_t ECU_ProtocolCrc32c(const void *data, size_t length)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFUL;
  size_t index;
  uint32_t bit;

  for (index = 0U; index < length; ++index)
  {
    crc ^= bytes[index];
    for (bit = 0U; bit < 8U; ++bit)
    {
      crc = (crc >> 1U) ^
            ((crc & 1U) ? 0x82F63B78UL : 0U);
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

size_t ECU_ProtocolEncodeV2(uint8_t message_type, uint16_t flags,
                            uint32_t sequence, uint32_t timestamp_ms,
                            const void *payload, uint16_t payload_size,
                            uint8_t *frame, size_t frame_capacity)
{
  ECU_V2Header header = {
    .magic = ECU_V2_MAGIC,
    .version = ECU_V2_VERSION,
    .message_type = message_type,
    .header_size = ECU_V2_HEADER_SIZE,
    .payload_size = payload_size,
    .flags = flags,
    .sequence = sequence,
    .timestamp_ms = timestamp_ms,
    .crc32c = 0U
  };
  size_t frame_size = sizeof(header) + payload_size;
  uint32_t crc;

  if ((frame == NULL) || (frame_size > frame_capacity) ||
      (frame_size > ECU_V2_MAX_FRAME_SIZE) ||
      ((payload_size != 0U) && (payload == NULL)))
  {
    return 0U;
  }
  memcpy(frame, &header, sizeof(header));
  if (payload_size != 0U)
  {
    memcpy(frame + sizeof(header), payload, payload_size);
  }
  crc = ECU_ProtocolCrc32c(frame, frame_size);
  memcpy(frame + offsetof(ECU_V2Header, crc32c), &crc, sizeof(crc));
  return frame_size;
}

bool ECU_ProtocolDecodeV2(const uint8_t *frame, size_t frame_length,
                          ECU_V2Header *header, void *payload,
                          size_t payload_capacity)
{
  ECU_V2Header decoded;
  uint8_t copy[ECU_V2_MAX_FRAME_SIZE];
  uint32_t expected_crc;
  uint32_t zero = 0U;

  if ((frame == NULL) || (header == NULL) ||
      (frame_length < sizeof(decoded)) ||
      (frame_length > sizeof(copy)))
  {
    return false;
  }
  memcpy(&decoded, frame, sizeof(decoded));
  if ((decoded.magic != ECU_V2_MAGIC) ||
      (decoded.version != ECU_V2_VERSION) ||
      (decoded.header_size != sizeof(decoded)) ||
      ((size_t)decoded.header_size + decoded.payload_size != frame_length) ||
      (decoded.payload_size > payload_capacity) ||
      ((decoded.payload_size != 0U) && (payload == NULL)))
  {
    return false;
  }
  memcpy(copy, frame, frame_length);
  expected_crc = decoded.crc32c;
  memcpy(copy + offsetof(ECU_V2Header, crc32c), &zero, sizeof(zero));
  if (ECU_ProtocolCrc32c(copy, frame_length) != expected_crc)
  {
    return false;
  }
  if (decoded.payload_size != 0U)
  {
    memcpy(payload, frame + sizeof(decoded), decoded.payload_size);
  }
  *header = decoded;
  return true;
}
