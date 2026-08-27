#include "ecu_network.h"

#include <limits.h>
#include <string.h>

#include "ecu_data_model.h"
#include "ecu_protocol.h"
#include "ethernetif.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "main.h"
#include "netif/ethernet.h"
#include "secure_nsc.h"

#define CONTROL_TIMEOUT_MS          250UL
#define CONTROL_APPLY_PERIOD_MS      20UL
#define STATUS_PERIOD_MS             50UL
#define DIAGNOSTIC_PERIOD_MS        100UL
#define LINK_POLL_PERIOD_MS         250UL
#define TELEMETRY_SEND_PERIOD_MS      8UL
#define CONTROL_PRIORITY_EMERGENCY  255U
#define CONTROL_MAX_SENDERS           6U
#define TUNING_SUBSCRIPTION_MAX_MS  2000UL

#define ECU_CAP_V2_UDP            (1UL << 0)
#define ECU_CAP_LWIP_STATIC       (1UL << 1)
#define ECU_CAP_CAN1_J1939        (1UL << 2)
#define ECU_CAP_CAN2_RESERVED     (1UL << 3)
#define ECU_CAP_SW_I2C_PCB_R1     (1UL << 4)
#define ECU_CAP_RELAY_OUTPUTS     (1UL << 6)
#define ECU_CAP_VALVE_CURRENT_PI  (1UL << 7)
#define ECU_CAP_VALVE_TUNING      (1UL << 8)

typedef struct
{
  bool active;
  bool sequence_valid;
  uint8_t sender_id;
  uint8_t priority;
  uint32_t last_sequence;
  uint32_t last_update_tick;
  ip_addr_t source_address;
  ECU_ControlPayloadV2 control;
} ControlSlot;

static struct netif ecu_netif;
static struct udp_pcb *control_pcb;
static struct udp_pcb *transmit_pcb;
static struct udp_pcb *tuning_pcb;
static ip_addr_t broadcast_address;
static ControlSlot slots[CONTROL_MAX_SENDERS];
static ControlSlot *active_slot;
static ECU_NetworkCounters counters;
static uint32_t last_apply_tick;
static uint32_t last_status_tick;
static uint32_t last_diagnostic_tick;
static uint32_t last_link_poll_tick;
static uint32_t last_telemetry_tick;
static uint32_t secure_command_sequence;
static uint8_t outputs_armed;
static ip_addr_t telemetry_client_address;
static uint16_t telemetry_client_port;
static uint32_t telemetry_subscription_deadline;
static uint32_t telemetry_dropped_baseline;
static uint8_t telemetry_dropped_baseline_valid;
static uint32_t transmit_sequence;

static bool Network_SequenceIsNewer(uint32_t value, uint32_t previous)
{
  uint32_t difference = value - previous;
  return (difference != 0U) && (difference < 0x80000000UL);
}

static bool Network_TimeExpired(uint32_t now, uint32_t last,
                                uint32_t timeout)
{
  /* Signed modular subtraction is wrap-safe for intervals below INT32_MAX.
     It also prevents a timestamp captured immediately before ethernet input
     from treating a callback timestamp one tick in the future as expired. */
  return (int32_t)(now - last) > (int32_t)timeout;
}

static bool Network_BooleanFieldsValid(const ECU_ControlPayloadV2 *control)
{
  const uint8_t values[] = {
    control->pump_enable, control->pump_select,
    control->headlamp_front_on, control->headlamp_rear_on,
    control->led_front_on, control->led_rear_on,
    control->buzzer_reverse_on, control->buzzer_main_on,
    control->indicator1_on, control->indicator2_on, control->indicator3_on,
    control->vib_original_on, control->vib_strong_on, control->vib_weak_on,
    control->vib_front_selected, control->vib_rear_selected,
    control->engine_start_request, control->power_latch_on,
    control->speed_mode_high, control->steering_enable,
    control->parking_brake_on, control->emergency_stop_request,
    control->main_power_relay_on, control->turn_signal_right_on,
    control->turn_signal_left_on
  };
  uint32_t index;

  for (index = 0U; index < sizeof(values); ++index)
  {
    if (values[index] > 1U) { return false; }
  }
  return true;
}

static bool Network_ControlValuesValid(const ECU_ControlPayloadV2 *control)
{
  return Network_BooleanFieldsValid(control) &&
         (control->engine_speed_level >= -3) &&
         (control->engine_speed_level <= 3) &&
         (control->valve_current_target_ma >= -SAFETY_VALVE_TARGET_MAX_MA) &&
         (control->valve_current_target_ma <= SAFETY_VALVE_TARGET_MAX_MA) &&
         (control->steering_target_tdeg >= -300) &&
         (control->steering_target_tdeg <= 300) &&
         (control->steering_speed_tdeg_per_s <= 6000U) &&
         !((control->vib_strong_on != 0U) &&
           (control->vib_weak_on != 0U));
}

static uint8_t Network_EffectivePriority(uint8_t sender_id,
                                         uint8_t requested_priority,
                                         bool emergency_requested)
{
  if (emergency_requested ||
      (requested_priority == CONTROL_PRIORITY_EMERGENCY))
  {
    return CONTROL_PRIORITY_EMERGENCY;
  }
  if ((sender_id >= 1U) && (sender_id <= 3U))
  {
    return sender_id;
  }
  return (requested_priority == 0U) ? 1U : requested_priority;
}

static ECU_ControlMode Network_ModeForSlot(const ControlSlot *slot)
{
  if (slot->priority == CONTROL_PRIORITY_EMERGENCY)
  {
    return ECU_CONTROL_EMERGENCY;
  }
  if (slot->sender_id == 1U) { return ECU_CONTROL_REMOTE; }
  if (slot->sender_id == 2U) { return ECU_CONTROL_OPERATOR; }
  if (slot->sender_id == 3U) { return ECU_CONTROL_AUTONOMOUS; }
  if (slot->priority >= 3U) { return ECU_CONTROL_AUTONOMOUS; }
  if (slot->priority >= 2U) { return ECU_CONTROL_OPERATOR; }
  return ECU_CONTROL_REMOTE;
}

static ControlSlot *Network_AcquireSlot(uint8_t sender_id,
                                        uint8_t effective_priority)
{
  ControlSlot *free_slot = NULL;
  ControlSlot *replacement = NULL;
  uint8_t lowest_priority = UINT8_MAX;
  uint32_t oldest_tick = UINT32_MAX;
  uint32_t index;

  for (index = 0U; index < CONTROL_MAX_SENDERS; ++index)
  {
    ControlSlot *slot = &slots[index];
    if (slot->active && (slot->sender_id == sender_id))
    {
      return slot;
    }
    if (!slot->active)
    {
      if (free_slot == NULL) { free_slot = slot; }
      continue;
    }
    if ((slot->priority < lowest_priority) ||
        ((slot->priority == lowest_priority) &&
         (slot->last_update_tick < oldest_tick)))
    {
      lowest_priority = slot->priority;
      oldest_tick = slot->last_update_tick;
      replacement = slot;
    }
  }
  if (free_slot != NULL)
  {
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->sender_id = sender_id;
    return free_slot;
  }
  if ((replacement != NULL) &&
      (effective_priority > replacement->priority))
  {
    memset(replacement, 0, sizeof(*replacement));
    replacement->sender_id = sender_id;
    return replacement;
  }
  return NULL;
}

static void Network_ExpireSlots(uint32_t now)
{
  uint32_t index;

  for (index = 0U; index < CONTROL_MAX_SENDERS; ++index)
  {
    ControlSlot *slot = &slots[index];
    if (slot->active &&
        Network_TimeExpired(now, slot->last_update_tick, CONTROL_TIMEOUT_MS))
    {
      slot->active = false;
      /* A sequence number belongs to one live control session. Once the
         watchdog expires, allow a restarted V2 client (which may start its
         sequence again at zero) to establish a new session. */
      slot->sequence_valid = false;
    }
  }
}

static ControlSlot *Network_FindActiveSlot(uint8_t sender_id)
{
  uint32_t index;

  for (index = 0U; index < CONTROL_MAX_SENDERS; ++index)
  {
    if (slots[index].active && (slots[index].sender_id == sender_id))
    {
      return &slots[index];
    }
  }
  return NULL;
}

static void Network_ReceiveControl(void *argument, struct udp_pcb *pcb,
                                   struct pbuf *packet,
                                   const ip_addr_t *address, u16_t port)
{
  uint8_t frame[ECU_V2_MAX_FRAME_SIZE];
  ECU_V2Header header;
  ECU_ControlDatagramV2 datagram;
  ControlSlot *slot;
  uint8_t effective_priority;
  bool emergency_requested;
  uint32_t now = HAL_GetTick();
  uint16_t frame_length = packet->tot_len;

  (void)argument;
  (void)pcb;
  (void)address;
  (void)port;
  if ((frame_length > sizeof(frame)) ||
      (pbuf_copy_partial(packet, frame, frame_length, 0U) != frame_length))
  {
    ++counters.invalid_control_frames;
    pbuf_free(packet);
    return;
  }
  pbuf_free(packet);
  if ((frame_length > 0U) && (frame[0] == 0xA5U))
  {
    ++counters.legacy_v1_frames_rejected;
    ++counters.rejected_control_frames;
    return;
  }
  memset(&datagram, 0, sizeof(datagram));
  if (!ECU_ProtocolDecodeV2(frame, frame_length, &header, &datagram,
                            sizeof(datagram)) ||
      (header.message_type != ECU_MESSAGE_CONTROL_COMMAND) ||
      (header.payload_size != sizeof(datagram)) ||
      (datagram.reserved != 0U))
  {
    ++counters.invalid_control_frames;
    return;
  }

  emergency_requested = (datagram.control.emergency_stop_request != 0U) ||
                        (datagram.priority == CONTROL_PRIORITY_EMERGENCY);
  Network_ExpireSlots(now);
  if (emergency_requested)
  {
    /* A structurally valid stop request is fail-safe dominant: stale
       sequence numbers, unrelated out-of-range actuator fields, and unknown
       optional flags must never turn an emergency request into motion. */
    slot = Network_FindActiveSlot(datagram.sender_id);
    if (slot == NULL)
    {
      slot = Network_AcquireSlot(datagram.sender_id,
                                 CONTROL_PRIORITY_EMERGENCY);
    }
    if (slot == NULL)
    {
      ++counters.rejected_control_frames;
      return;
    }
    slot->active = true;
    slot->sequence_valid = true;
    slot->sender_id = datagram.sender_id;
    slot->priority = CONTROL_PRIORITY_EMERGENCY;
    slot->last_sequence = header.sequence;
    slot->last_update_tick = now;
    ip_addr_copy(slot->source_address, *address);
    memset(&slot->control, 0, sizeof(slot->control));
    ++counters.valid_control_frames;
    return;
  }
  effective_priority = Network_EffectivePriority(datagram.sender_id,
                                                 datagram.priority,
                                                 emergency_requested);
  if (((header.flags & ~ECU_CONTROL_ALLOWED_FLAGS) != 0U) ||
      !Network_ControlValuesValid(&datagram.control))
  {
    ++counters.rejected_control_frames;
    return;
  }

  slot = Network_FindActiveSlot(datagram.sender_id);
  if ((slot != NULL) && slot->sequence_valid &&
      !Network_SequenceIsNewer(header.sequence, slot->last_sequence))
  {
    ++counters.rejected_control_frames;
    return;
  }
  if ((header.flags & ECU_CONTROL_FLAG_RELEASE) != 0U)
  {
    if (!ECU_DataModelControlIsNeutral(&datagram.control))
    {
      ++counters.rejected_control_frames;
      return;
    }
    if (slot != NULL)
    {
      slot->active = false;
      slot->sequence_valid = false;
    }
    ++counters.valid_control_frames;
    return;
  }
  if ((header.flags & ECU_CONTROL_FLAG_CLEAR_FAULT) != 0U)
  {
    if (!ECU_DataModelControlIsNeutral(&datagram.control) ||
        (SECURE_SafetyClearFault(SAFETY_CLEAR_FAULT_TOKEN) != SAFETY_RESULT_OK))
    {
      ++counters.rejected_control_frames;
      return;
    }
  }
  if (slot == NULL)
  {
    slot = Network_AcquireSlot(datagram.sender_id, effective_priority);
    if (slot == NULL)
    {
      ++counters.rejected_control_frames;
      return;
    }
  }

  slot->active = true;
  slot->sequence_valid = true;
  slot->sender_id = datagram.sender_id;
  /* Preset senders 1..3 use their fixed levels; custom sender IDs keep
     their requested (non-emergency) priority. */
  slot->priority = effective_priority;
  slot->last_sequence = header.sequence;
  slot->last_update_tick = now;
  ip_addr_copy(slot->source_address, *address);
  slot->control = datagram.control;
  ++counters.valid_control_frames;
}

static ControlSlot *Network_SelectAuthority(uint32_t now)
{
  ControlSlot *selected = NULL;
  uint32_t index;

  Network_ExpireSlots(now);
  for (index = 0U; index < CONTROL_MAX_SENDERS; ++index)
  {
    ControlSlot *slot = &slots[index];
    if (!slot->active) { continue; }
    if ((selected == NULL) || (slot->priority > selected->priority) ||
        ((slot->priority == selected->priority) &&
         (slot->sender_id < selected->sender_id)))
    {
      selected = slot;
    }
  }
  return selected;
}

static void Network_ApplyAuthority(uint32_t now)
{
  ControlSlot *selected = Network_SelectAuthority(now);
  bool selection_changed = selected != active_slot;

  if (selection_changed)
  {
    active_slot = selected;
    ++counters.authority_switches;
    last_apply_tick = now - CONTROL_APPLY_PERIOD_MS;
  }
  if (selected == NULL)
  {
    /* Also clear the reported authority after an emergency-only slot expires.
       In that state the outputs are already disarmed, so testing outputs_armed
       alone would leave V2 status stuck in emergency mode. */
    if (selection_changed || (outputs_armed != 0U))
    {
      ECU_DataModelControlLost();
      outputs_armed = 0U;
    }
    return;
  }

  ECU_DataModelSetAuthority(Network_ModeForSlot(selected), selected->sender_id);
  if (selected->priority == CONTROL_PRIORITY_EMERGENCY)
  {
    if (outputs_armed != 0U)
    {
      (void)SECURE_SafetyDisarmOutputs();
      outputs_armed = 0U;
    }
    return;
  }
  if ((uint32_t)(now - last_apply_tick) < CONTROL_APPLY_PERIOD_MS)
  {
    return;
  }
  last_apply_tick = now;

  if (outputs_armed == 0U)
  {
    if (SECURE_SafetyArmOutputs(SAFETY_ARM_TOKEN) != SAFETY_RESULT_OK)
    {
      return;
    }
    outputs_armed = 1U;
  }
  ++secure_command_sequence;
  if (ECU_DataModelApplyControl(&selected->control,
                                secure_command_sequence) != SAFETY_RESULT_OK)
  {
    (void)SECURE_SafetyDisarmOutputs();
    outputs_armed = 0U;
  }
}

static bool Network_SendV2(struct udp_pcb *pcb, const ip_addr_t *address,
                           uint16_t port, uint8_t message_type,
                           uint16_t flags, const void *payload,
                           uint16_t payload_size)
{
  uint8_t frame[ECU_V2_MAX_FRAME_SIZE];
  size_t frame_length;
  struct pbuf *packet;
  err_t result;

  ++transmit_sequence;
  frame_length = ECU_ProtocolEncodeV2(
      message_type, flags, transmit_sequence, HAL_GetTick(), payload,
      payload_size, frame, sizeof(frame));
  if (frame_length == 0U) { return false; }
  packet = pbuf_alloc(PBUF_TRANSPORT, (u16_t)frame_length, PBUF_RAM);
  if (packet == NULL) { return false; }
  if (pbuf_take(packet, frame, frame_length) != ERR_OK)
  {
    pbuf_free(packet);
    return false;
  }
  result = udp_sendto(pcb, packet, address, port);
  pbuf_free(packet);
  return result == ERR_OK;
}

static void Network_SendStatus(void)
{
  ECU_DataModelUpdateStatus();
  if (Network_SendV2(transmit_pcb, &broadcast_address, ECU_STATUS_PORT,
                     ECU_MESSAGE_STATUS, 0U, ECU_DataModelGetStatus(),
                     sizeof(ECU_StatusPayloadV2)))
  {
    ++counters.status_frames_sent;
  }
}

static void Network_SendDiagnostic(void)
{
  ECU_DiagnosticPayloadV2 diagnostic = {0};
  const SAFETY_AdcSnapshot *adc = ECU_DataModelGetAdcSnapshot();
  const SAFETY_ActuatorSnapshot *actuator =
      ECU_DataModelGetActuatorSnapshot();
  const ECU_StatusPayloadV2 *status = ECU_DataModelGetStatus();

  diagnostic.capability_flags = ECU_CAP_V2_UDP | ECU_CAP_LWIP_STATIC |
      ECU_CAP_CAN1_J1939 | ECU_CAP_CAN2_RESERVED | ECU_CAP_SW_I2C_PCB_R1 |
      ECU_CAP_RELAY_OUTPUTS | ECU_CAP_VALVE_CURRENT_PI |
      ECU_CAP_VALVE_TUNING;
  diagnostic.safety_status = actuator->status;
  diagnostic.requested_relay_mask = actuator->requested_relay_mask;
  diagnostic.applied_relay_mask = actuator->applied_relay_mask;
  diagnostic.secure_uptime_ms = actuator->secure_uptime_ms;
  diagnostic.command_age_ms = actuator->command_age_ms;
  diagnostic.valid_control_frames = counters.valid_control_frames;
  diagnostic.invalid_control_frames = counters.invalid_control_frames;
  diagnostic.rejected_control_frames = counters.rejected_control_frames;
  diagnostic.authority_switches = counters.authority_switches;
  diagnostic.legacy_v1_frames_rejected =
      counters.legacy_v1_frames_rejected;
  diagnostic.telemetry_frames_sent = counters.telemetry_frames_sent;
  memcpy(diagnostic.slow_adc, adc->slow_adc, sizeof(diagnostic.slow_adc));
  memcpy(diagnostic.current_adc, adc->current_adc,
         sizeof(diagnostic.current_adc));
  diagnostic.engine_start_remaining_ms = actuator->engine_start_remaining_ms;
  diagnostic.engine_speed_remaining = actuator->engine_speed_remaining;
  diagnostic.engine_speed_state = actuator->engine_speed_state;
  diagnostic.link_up = netif_is_link_up(&ecu_netif) ? 1U : 0U;
  diagnostic.active_sender_id = status->active_sender_id;
  diagnostic.control_mode = status->control_mode;
  diagnostic.software_i2c_bus_ok = actuator->software_i2c_bus_ok;
  diagnostic.valve_fault_flags = actuator->valve_fault_flags;
  diagnostic.telemetry_dropped_samples =
      counters.telemetry_dropped_samples;
  if (Network_SendV2(transmit_pcb, &broadcast_address,
                     ECU_DIAGNOSTIC_PORT, ECU_MESSAGE_DIAGNOSTIC, 0U,
                     &diagnostic, sizeof(diagnostic)))
  {
    ++counters.diagnostic_frames_sent;
  }
}

static bool Network_TuningAuthorized(const ip_addr_t *address)
{
  return (active_slot == NULL) || !active_slot->active ||
         ip_addr_cmp(address, &active_slot->source_address);
}

static void Network_SendConfigReply(const ip_addr_t *address, uint16_t port,
                                    uint32_t request_sequence,
                                    int32_t result)
{
  ECU_ValveConfigReplyPayload reply = {0};
  SAFETY_ValveConfigSnapshot snapshot;

  reply.result = result;
  reply.request_sequence = request_sequence;
  if (SECURE_SafetyGetValveConfig(&snapshot) != SAFETY_RESULT_OK)
  {
    reply.result = SAFETY_RESULT_INTERNAL_ERROR;
  }
  else
  {
    memcpy(&reply.snapshot, &snapshot, sizeof(snapshot));
  }
  (void)Network_SendV2(tuning_pcb, address, port,
                       ECU_MESSAGE_VALVE_CONFIG_REPLY, 0U, &reply,
                       sizeof(reply));
}

static void Network_SendOperationAck(const ip_addr_t *address, uint16_t port,
                                     uint32_t request_sequence,
                                     int32_t result)
{
  ECU_OperationAckPayload ack = {
    .result = result,
    .request_sequence = request_sequence
  };
  (void)Network_SendV2(tuning_pcb, address, port,
                       ECU_MESSAGE_OPERATION_ACK, 0U, &ack, sizeof(ack));
}

static void Network_ReceiveTuning(void *argument, struct udp_pcb *pcb,
                                  struct pbuf *packet,
                                  const ip_addr_t *address, u16_t port)
{
  union
  {
    SAFETY_ValveConfig config;
    ECU_TelemetrySubscribePayload subscribe;
    uint8_t bytes[sizeof(SAFETY_ValveConfig)];
  } payload;
  uint8_t frame[ECU_V2_MAX_FRAME_SIZE];
  ECU_V2Header header;
  uint16_t frame_length = packet->tot_len;
  int32_t result = SAFETY_RESULT_BAD_ARGUMENT;

  (void)argument;
  (void)pcb;
  if ((frame_length > sizeof(frame)) ||
      (pbuf_copy_partial(packet, frame, frame_length, 0U) != frame_length))
  {
    pbuf_free(packet);
    return;
  }
  pbuf_free(packet);
  memset(&payload, 0, sizeof(payload));
  if (!ECU_ProtocolDecodeV2(frame, frame_length, &header, &payload,
                            sizeof(payload)))
  {
    return;
  }

  Network_ExpireSlots(HAL_GetTick());
  switch (header.message_type)
  {
    case ECU_MESSAGE_VALVE_CONFIG_GET:
      if (header.payload_size == 0U)
      {
        Network_SendConfigReply(address, port, header.sequence,
                                SAFETY_RESULT_OK);
      }
      break;

    case ECU_MESSAGE_VALVE_CONFIG_APPLY:
      if ((header.payload_size == sizeof(payload.config)) &&
          Network_TuningAuthorized(address))
      {
        result = SECURE_SafetyApplyValveConfig(&payload.config);
      }
      else if (!Network_TuningAuthorized(address))
      {
        result = SAFETY_RESULT_CONFLICT;
      }
      Network_SendConfigReply(address, port, header.sequence, result);
      break;

    case ECU_MESSAGE_VALVE_CONFIG_SAVE:
      if ((header.payload_size == 0U) && Network_TuningAuthorized(address))
      {
        result = SECURE_SafetySaveValveConfig();
      }
      else if (!Network_TuningAuthorized(address))
      {
        result = SAFETY_RESULT_CONFLICT;
      }
      Network_SendConfigReply(address, port, header.sequence, result);
      break;

    case ECU_MESSAGE_VALVE_CONFIG_RELOAD:
      if ((header.payload_size == 0U) && Network_TuningAuthorized(address))
      {
        result = SECURE_SafetyReloadValveConfig();
      }
      else if (!Network_TuningAuthorized(address))
      {
        result = SAFETY_RESULT_CONFLICT;
      }
      Network_SendConfigReply(address, port, header.sequence, result);
      break;

    case ECU_MESSAGE_TELEMETRY_SUBSCRIBE:
      if ((header.payload_size == sizeof(payload.subscribe)) &&
          Network_TuningAuthorized(address) &&
          (payload.subscribe.destination_port == ECU_TELEMETRY_PORT) &&
          (payload.subscribe.sample_rate_hz == 1000U) &&
          (payload.subscribe.ttl_ms >= 500U) &&
          (payload.subscribe.ttl_ms <= TUNING_SUBSCRIPTION_MAX_MS))
      {
        ip_addr_copy(telemetry_client_address, *address);
        telemetry_client_port = payload.subscribe.destination_port;
        telemetry_subscription_deadline =
            HAL_GetTick() + payload.subscribe.ttl_ms;
        telemetry_dropped_baseline_valid = 0U;
        result = SAFETY_RESULT_OK;
      }
      else if (!Network_TuningAuthorized(address))
      {
        result = SAFETY_RESULT_CONFLICT;
      }
      Network_SendOperationAck(address, port, header.sequence, result);
      break;

    default:
      Network_SendOperationAck(address, port, header.sequence,
                               SAFETY_RESULT_UNSUPPORTED_VERSION);
      break;
  }
}

static void Network_SendValveTelemetry(uint32_t now)
{
  SAFETY_ValveTelemetryBatch batch;
  uint16_t payload_size;

  if ((telemetry_client_port == 0U) ||
      ((int32_t)(now - telemetry_subscription_deadline) >= 0))
  {
    telemetry_client_port = 0U;
    telemetry_dropped_baseline_valid = 0U;
    return;
  }
  if ((SECURE_SafetyReadValveTelemetry(&batch) != SAFETY_RESULT_OK) ||
      (batch.sample_count == 0U))
  {
    return;
  }
  /* The Secure ring runs continuously so pre-subscription overwrites are not
     transport loss. Report a session-relative counter to make the tuning UI
     and acceptance logs distinguish live loss from an idle historical count. */
  if (telemetry_dropped_baseline_valid == 0U)
  {
    telemetry_dropped_baseline = batch.dropped_samples;
    telemetry_dropped_baseline_valid = 1U;
  }
  batch.dropped_samples -= telemetry_dropped_baseline;
  counters.telemetry_dropped_samples = batch.dropped_samples;
  payload_size = (uint16_t)(offsetof(SAFETY_ValveTelemetryBatch, samples) +
      ((uint32_t)batch.sample_count * sizeof(batch.samples[0])));
  if (Network_SendV2(transmit_pcb, &telemetry_client_address,
                     telemetry_client_port, ECU_MESSAGE_VALVE_TELEMETRY, 0U,
                     &batch, payload_size))
  {
    ++counters.telemetry_frames_sent;
  }
}

bool ECU_NetworkInit(void)
{
  ip4_addr_t ip;
  ip4_addr_t mask;
  ip4_addr_t gateway;
  err_t bind_result;

  memset(&ecu_netif, 0, sizeof(ecu_netif));
  memset(slots, 0, sizeof(slots));
  memset(&counters, 0, sizeof(counters));
  active_slot = NULL;
  outputs_armed = 0U;
  secure_command_sequence = 0U;
  telemetry_client_port = 0U;
  telemetry_subscription_deadline = 0U;
  telemetry_dropped_baseline = 0U;
  telemetry_dropped_baseline_valid = 0U;
  last_telemetry_tick = HAL_GetTick();
  transmit_sequence = 0U;

  lwip_init();
  IP4_ADDR(&ip, ECU_IP_ADDRESS_0, ECU_IP_ADDRESS_1,
           ECU_IP_ADDRESS_2, ECU_IP_ADDRESS_3);
  IP4_ADDR(&mask, 255U, 255U, 0U, 0U);
  IP4_ADDR(&gateway, 172U, 16U, 0U, 1U);
  IP4_ADDR(ip_2_ip4(&broadcast_address), 172U, 16U, 255U, 255U);
  IP_SET_TYPE_VAL(broadcast_address, IPADDR_TYPE_V4);

  if (netif_add(&ecu_netif, &ip, &mask, &gateway, NULL,
                ethernetif_init, ethernet_input) == NULL)
  {
    return false;
  }
  netif_set_default(&ecu_netif);

  control_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
  transmit_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
  tuning_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
  if ((control_pcb == NULL) || (transmit_pcb == NULL) ||
      (tuning_pcb == NULL))
  {
    if (control_pcb != NULL) { udp_remove(control_pcb); }
    if (transmit_pcb != NULL) { udp_remove(transmit_pcb); }
    if (tuning_pcb != NULL) { udp_remove(tuning_pcb); }
    control_pcb = NULL;
    transmit_pcb = NULL;
    tuning_pcb = NULL;
    return false;
  }
  /* Keep UDP/50001 as both the status destination and source port so receivers
     can validate the source tuple consistently. */
  ip_set_option(control_pcb, SOF_BROADCAST);
  ip_set_option(transmit_pcb, SOF_BROADCAST);
  bind_result = udp_bind(control_pcb, IP_ANY_TYPE, ECU_CONTROL_PORT);
  if (bind_result == ERR_OK)
  {
    bind_result = udp_bind(transmit_pcb, IP_ANY_TYPE, ECU_STATUS_PORT);
  }
  if (bind_result == ERR_OK)
  {
    bind_result = udp_bind(tuning_pcb, IP_ANY_TYPE, ECU_TUNING_PORT);
  }
  if (bind_result != ERR_OK)
  {
    udp_remove(control_pcb);
    udp_remove(transmit_pcb);
    udp_remove(tuning_pcb);
    control_pcb = NULL;
    transmit_pcb = NULL;
    tuning_pcb = NULL;
    return false;
  }
  udp_recv(control_pcb, Network_ReceiveControl, NULL);
  udp_recv(tuning_pcb, Network_ReceiveTuning, NULL);
  last_apply_tick = HAL_GetTick();
  last_status_tick = HAL_GetTick();
  last_diagnostic_tick = HAL_GetTick();
  last_link_poll_tick = HAL_GetTick();
  return true;
}

void ECU_NetworkProcess(void)
{
  uint32_t now;

  ethernetif_input(&ecu_netif);
  sys_check_timeouts();
  /* Receive callbacks timestamp control slots while ethernetif_input runs.
     Sample the loop time afterwards so authority expiry never compares those
     timestamps against an older tick. */
  now = HAL_GetTick();
  if ((now - last_link_poll_tick) >= LINK_POLL_PERIOD_MS)
  {
    last_link_poll_tick = now;
    ethernet_link_check_state(&ecu_netif);
  }
  Network_ApplyAuthority(now);
  if ((uint32_t)(now - last_telemetry_tick) >= TELEMETRY_SEND_PERIOD_MS)
  {
    last_telemetry_tick = now;
    Network_SendValveTelemetry(now);
  }
  if ((now - last_status_tick) >= STATUS_PERIOD_MS)
  {
    last_status_tick = now;
    Network_SendStatus();
  }
  if ((now - last_diagnostic_tick) >= DIAGNOSTIC_PERIOD_MS)
  {
    last_diagnostic_tick = now;
    Network_SendDiagnostic();
  }
}

bool ECU_NetworkLinkIsUp(void)
{
  return netif_is_link_up(&ecu_netif);
}

const ECU_NetworkCounters *ECU_NetworkGetCounters(void)
{
  return &counters;
}
