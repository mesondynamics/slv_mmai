#include "ecu_network.h"

#include <limits.h>
#include <string.h>

#include "ecu_data_model.h"
#include "ecu_protocol.h"
#include "control_authority_policy.h"
#include "ethernet_tx_scheduler.h"
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
#define STEERING_STATUS_PERIOD_MS   100UL
#define STEERING_STATUS_PHASE_MS     10UL
#define DIAGNOSTIC_PERIOD_MS        100UL
#define DIAGNOSTIC_PHASE_MS          35UL
#define SECURITY_STATUS_PERIOD_MS   100UL
#define SECURITY_STATUS_PHASE_MS     60UL
#define LINK_POLL_PERIOD_MS         250UL
#define TELEMETRY_SEND_PERIOD_MS      8UL
#define CONTROL_PRIORITY_EMERGENCY  255U
#define CONTROL_MAX_SENDERS           6U
#define TUNING_SUBSCRIPTION_MAX_MS  2000UL
#define OTA_RESET_DELAY_MS           500UL
/* Preserve the previous passive 20 Hz A5 status stream. Periodic V1, V2,
   steering, diagnostic and security frames use distinct phases so normal
   operation never consumes the four Ethernet TX descriptors in one burst. */
#define LEGACY_STATUS_PERIOD_MS        50UL
#define LEGACY_STATUS_PHASE_MS         25UL

#define ECU_CAP_V2_UDP            (1UL << 0)
#define ECU_CAP_LWIP_STATIC       (1UL << 1)
#define ECU_CAP_CAN1_J1939        (1UL << 2)
#define ECU_CAP_CAN2_STEERING_RATE (1UL << 3)
#define ECU_CAP_SW_I2C_PCB_R1     (1UL << 4)
#define ECU_CAP_ATECC608_PROBE     (1UL << 5)
#define ECU_CAP_RELAY_OUTPUTS     (1UL << 6)
#define ECU_CAP_VALVE_CURRENT_PI  (1UL << 7)
#define ECU_CAP_VALVE_TUNING      (1UL << 8)
#define ECU_CAP_SIGNED_ETHERNET_OTA (1UL << 9)
#define ECU_CAP_LEGACY_V1_SAFE_CONTROL (1UL << 10)
#define ECU_CAP_LATCHED_ESTOP_RESET (1UL << 11)

typedef struct
{
  bool active;
  bool sequence_valid;
  bool rearm_candidate;
  uint8_t sender_id;
  uint8_t priority;
  uint16_t flags;
  uint32_t session_generation;
  uint32_t last_sequence;
  uint32_t last_update_tick;
  ip_addr_t source_address;
  ECU_ControlPayloadV2 control;
} ControlSlot;

static struct netif ecu_netif;
static struct udp_pcb *control_pcb;
static struct udp_pcb *transmit_pcb;
static struct udp_pcb *tuning_pcb;
static struct udp_pcb *ota_pcb;
static ip_addr_t broadcast_address;
static ControlSlot slots[CONTROL_MAX_SENDERS];
static ControlSlot *active_slot;
static uint32_t active_session_generation;
static ControlAuthorityPolicy authority_policy;
static ECU_NetworkCounters counters;
static uint32_t last_apply_tick;
static uint32_t last_link_poll_tick;
static uint32_t secure_command_sequence;
static uint8_t outputs_armed;
static ip_addr_t telemetry_client_address;
static uint16_t telemetry_client_port;
static uint32_t telemetry_subscription_deadline;
static uint32_t telemetry_dropped_baseline;
static uint8_t telemetry_dropped_baseline_valid;
static uint32_t transmit_sequence;
static uint32_t ota_reset_deadline;
static uint8_t ota_reset_pending;
static uint8_t network_operational;
static EthernetTxScheduler transmit_scheduler;

static const EthernetTxScheduleConfig transmit_schedule[
    ETHERNET_TX_CATEGORY_COUNT] = {
  [ETHERNET_TX_CATEGORY_STATUS] = {
    .period_ms = STATUS_PERIOD_MS,
    .phase_ms = 0U
  },
  [ETHERNET_TX_CATEGORY_STEERING] = {
    .period_ms = STEERING_STATUS_PERIOD_MS,
    .phase_ms = STEERING_STATUS_PHASE_MS
  },
  [ETHERNET_TX_CATEGORY_DIAGNOSTIC] = {
    .period_ms = DIAGNOSTIC_PERIOD_MS,
    .phase_ms = DIAGNOSTIC_PHASE_MS
  },
  [ETHERNET_TX_CATEGORY_SECURITY] = {
    .period_ms = SECURITY_STATUS_PERIOD_MS,
    .phase_ms = SECURITY_STATUS_PHASE_MS
  },
  [ETHERNET_TX_CATEGORY_LEGACY] = {
    .period_ms = LEGACY_STATUS_PERIOD_MS,
    .phase_ms = LEGACY_STATUS_PHASE_MS
  },
  [ETHERNET_TX_CATEGORY_TELEMETRY] = {
    .period_ms = TELEMETRY_SEND_PERIOD_MS,
    .phase_ms = 0U
  }
};

static bool Network_SequenceIsNewer(uint32_t value, uint32_t previous)
{
  uint32_t difference = value - previous;
  return (difference != 0U) && (difference < 0x80000000UL);
}

static bool Network_TimeExpired(uint32_t now, uint32_t last,
                                uint32_t timeout)
{
  return ControlAuthorityPolicy_TimeExpired(now, last, timeout);
}

static bool Network_AddressMatches(const ip_addr_t *address,
                                   uint8_t octet_0, uint8_t octet_1,
                                   uint8_t octet_2, uint8_t octet_3)
{
  ip_addr_t expected;

  if (address == NULL)
  {
    return false;
  }
  IP4_ADDR(ip_2_ip4(&expected), octet_0, octet_1, octet_2, octet_3);
  IP_SET_TYPE_VAL(expected, IPADDR_TYPE_V4);
  return ip_addr_cmp(address, &expected);
}

static bool Network_ServiceHostAuthorized(const ip_addr_t *address)
{
  return Network_AddressMatches(
      address, ECU_SERVICE_HOST_ADDRESS_0, ECU_SERVICE_HOST_ADDRESS_1,
      ECU_SERVICE_HOST_ADDRESS_2, ECU_SERVICE_HOST_ADDRESS_3);
}

static bool Network_ControlHostAuthorized(const ip_addr_t *address)
{
  return Network_AddressMatches(
             address, ECU_REMOTE_HOST_ADDRESS_0, ECU_REMOTE_HOST_ADDRESS_1,
             ECU_REMOTE_HOST_ADDRESS_2, ECU_REMOTE_HOST_ADDRESS_3) ||
         Network_ServiceHostAuthorized(address) ||
         Network_AddressMatches(
             address, ECU_DOMAIN_HOST_ADDRESS_0, ECU_DOMAIN_HOST_ADDRESS_1,
             ECU_DOMAIN_HOST_ADDRESS_2, ECU_DOMAIN_HOST_ADDRESS_3);
}

static bool Network_SenderCanControlSteering(uint8_t sender_id)
{
  /* Secure steering ownership has exactly three distinct identities. Never
     collapse a custom network sender onto one of those identities, because a
     same-source update would bypass the mandatory zero/disable handover. */
  return (sender_id >= 1U) && (sender_id <= 3U);
}

static bool Network_ControlRequestsSteering(
    uint16_t flags, const ECU_ControlPayloadV2 *control)
{
  return ((flags & ECU_CONTROL_FLAG_STEERING_RATE) != 0U) ||
         (control->steering_enable != 0U) ||
         (control->steering_target_tdeg != 0) ||
         (control->steering_speed_tdeg_per_s != 0U);
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
  if (requested_priority != 0U)
  {
    return requested_priority;
  }
  /* Preserve old clients that sent zero: the three canonical identities fall
     back to their historical levels, while custom identities use level 1. */
  return ((sender_id >= 1U) && (sender_id <= 3U)) ? sender_id : 1U;
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

static void Network_StartSlotSession(ControlSlot *slot, uint8_t sender_id)
{
  memset(slot, 0, sizeof(*slot));
  slot->sender_id = sender_id;
  slot->session_generation =
      ControlAuthorityPolicy_BeginSession(&authority_policy);
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
    Network_StartSlotSession(free_slot, sender_id);
    return free_slot;
  }
  if ((replacement != NULL) &&
      (effective_priority > replacement->priority))
  {
    Network_StartSlotSession(replacement, sender_id);
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
      uint8_t expired_sender = slot->sender_id;
      bool active_authority_expired =
          (slot == active_slot) &&
          (slot->session_generation == active_session_generation);

      slot->active = false;
      slot->rearm_candidate = false;
      /* A sequence number belongs to one live control session. Once the
         watchdog expires, allow a restarted V2 client to start its sequence
         again, but never allow it to restore motion before a neutral-disabled
         rearm command has actually crossed the Secure boundary. */
      slot->sequence_valid = false;
      if (active_authority_expired)
      {
        /* This latch survives immediate reuse of the same ControlSlot storage
           by ethernetif_input(). Network_ApplyAuthority() must first process a
           complete safe round before considering any newly received session. */
        ControlAuthorityPolicy_MarkAuthorityExpired(&authority_policy,
                                                     expired_sender);
      }
      else
      {
        ControlAuthorityPolicy_RequireRearm(&authority_policy,
                                             expired_sender);
      }
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

static void Network_RequireRearmForAllNormalSenders(void)
{
  uint32_t index;

  for (index = 0U; index < CONTROL_MAX_SENDERS; ++index)
  {
    ControlSlot *slot = &slots[index];
    if (slot->active && (slot->priority != CONTROL_PRIORITY_EMERGENCY))
    {
      ControlAuthorityPolicy_RequireRearm(&authority_policy,
                                           slot->sender_id);
      slot->rearm_candidate = false;
    }
  }
}

static bool Network_OtherEmergencyIsLive(const ControlSlot *owner)
{
  uint32_t index;

  for (index = 0U; index < CONTROL_MAX_SENDERS; ++index)
  {
    if (slots[index].active &&
        (slots[index].priority == CONTROL_PRIORITY_EMERGENCY) &&
        (&slots[index] != owner))
    {
      return true;
    }
  }
  return false;
}

static bool Network_ResetNetworkEStop(ECU_ControlDatagramV2 *datagram,
                                      uint16_t flags, uint32_t sequence,
                                      const ip_addr_t *address, uint32_t now)
{
  ControlSlot *reset_session;
  int32_t result;

  Network_ExpireSlots(now);
  if ((flags != ECU_CONTROL_FLAG_ESTOP_RESET) ||
      (datagram->priority == CONTROL_PRIORITY_EMERGENCY) ||
      !Network_ControlHostAuthorized(address) ||
      !ECU_DataModelControlIsNeutral(&datagram->control))
  {
    ++counters.rejected_control_frames;
    return false;
  }

  reset_session = Network_FindActiveSlot(datagram->sender_id);
  if ((reset_session == NULL) ||
      !ip_addr_cmp(address, &reset_session->source_address) ||
      !reset_session->sequence_valid ||
      !Network_SequenceIsNewer(sequence, reset_session->last_sequence))
  {
    /* A reset is never allowed to create a session.  A controller resetting
       somebody else's expired/orphaned latch must first send a normal,
       neutral-disabled frame.  This binds the reset to a live sender/IP
       session and a strictly newer sequence, so a captured reset datagram
       cannot clear a later E-stop latch. */
    ++counters.rejected_control_frames;
    return false;
  }
  if (Network_OtherEmergencyIsLive(
          (reset_session->priority == CONTROL_PRIORITY_EMERGENCY) ?
          reset_session : NULL))
  {
    ++counters.rejected_control_frames;
    return false;
  }

  result = SECURE_SafetyResetNetworkEStop(
      SAFETY_NETWORK_ESTOP_RESET_TOKEN);
  if (result != SAFETY_RESULT_OK)
  {
    ++counters.rejected_control_frames;
    return false;
  }
  /* RESET consumes and destroys its source binding.  Keeping the neutral
     session alive would let Network_ApplyAuthority() arm the ordinary
     takeover-relay baseline for up to 250 ms, whereas an E-stop reset is
     required to restore K12 only and must never grant control authority. */
  Network_RequireRearmForAllNormalSenders();
  ControlAuthorityPolicy_RequireRearm(&authority_policy,
                                       reset_session->sender_id);
  reset_session->active = false;
  reset_session->sequence_valid = false;
  reset_session->rearm_candidate = false;
  ControlAuthorityPolicy_RequireSafeRound(&authority_policy);
  active_slot = NULL;
  active_session_generation = 0U;
  outputs_armed = 0U;
  ECU_DataModelControlLost();
  ++counters.valid_control_frames;
  return true;
}

static bool Network_AcceptControl(ECU_ControlDatagramV2 *datagram,
                                  uint16_t flags, uint32_t sequence,
                                  const ip_addr_t *address, uint32_t now)
{
  ControlSlot *slot;
  uint8_t effective_priority;
  bool emergency_requested;
  bool neutral_disabled;
  bool rearm_required;

  emergency_requested = (datagram->control.emergency_stop_request != 0U) ||
                        (datagram->priority == CONTROL_PRIORITY_EMERGENCY);
  Network_ExpireSlots(now);
  if (!emergency_requested &&
      (flags == ECU_CONTROL_FLAG_ESTOP_RESET) &&
      ECU_DataModelControlIsNeutral(&datagram->control))
  {
    return Network_ResetNetworkEStop(datagram, flags, sequence, address, now);
  }
  if (emergency_requested)
  {
    /* A structurally valid stop request is fail-safe dominant: stale
       sequence numbers, unrelated out-of-range actuator fields, and unknown
       optional flags must never turn an emergency request into motion. */
    (void)SECURE_SafetyAssertNetworkEStop();
    outputs_armed = 0U;
    Network_RequireRearmForAllNormalSenders();
    slot = Network_FindActiveSlot(datagram->sender_id);
    if (slot == NULL)
    {
      slot = Network_AcquireSlot(datagram->sender_id,
                                 CONTROL_PRIORITY_EMERGENCY);
    }
    if (slot == NULL)
    {
      ++counters.rejected_control_frames;
      return false;
    }
    slot->active = true;
    slot->sequence_valid = true;
    slot->rearm_candidate = false;
    slot->sender_id = datagram->sender_id;
    slot->priority = CONTROL_PRIORITY_EMERGENCY;
    slot->flags = 0U;
    slot->last_sequence = sequence;
    slot->last_update_tick = now;
    ip_addr_copy(slot->source_address, *address);
    memset(&slot->control, 0, sizeof(slot->control));
    ++counters.valid_control_frames;
    return true;
  }
  if ((flags & ECU_CONTROL_FLAG_ESTOP_RESET) != 0U)
  {
    /* ESTOP_RESET is a standalone neutral transaction. Any flag combination
       or non-emergency payload is rejected without creating authority. */
    ++counters.rejected_control_frames;
    return false;
  }
  /* Normal motion is confined to the serial-derived domain controller plus
     the fleet-wide service and remote-controller addresses. This is defense
     in depth, not cryptographic authentication; the structurally valid
     emergency path above intentionally remains dominant. */
  if (!Network_ControlHostAuthorized(address))
  {
    ++counters.rejected_control_frames;
    return false;
  }
  effective_priority = Network_EffectivePriority(datagram->sender_id,
                                                 datagram->priority,
                                                 emergency_requested);
  if (!ECU_ProtocolControlValuesValid(flags, &datagram->control))
  {
    ++counters.rejected_control_frames;
    return false;
  }
  if (Network_ControlRequestsSteering(flags, &datagram->control) &&
      !Network_SenderCanControlSteering(datagram->sender_id))
  {
    ++counters.rejected_control_frames;
    return false;
  }
  ECU_ProtocolNormalizeControl(flags, &datagram->control);
  neutral_disabled = ECU_DataModelControlIsNeutral(&datagram->control);
  rearm_required = ControlAuthorityPolicy_RearmRequired(
      &authority_policy, datagram->sender_id);
  if (!ControlAuthorityPolicy_CanEstablish(
          &authority_policy, datagram->sender_id, neutral_disabled))
  {
    /* A lease that has crossed 250 ms is a new control session. Reject the
       whole non-neutral frame without acquiring or refreshing a slot; only a
       neutral-disabled frame may become the rearm candidate. */
    ++counters.rejected_control_frames;
    return false;
  }

  slot = Network_FindActiveSlot(datagram->sender_id);
  if ((slot != NULL) &&
      !ip_addr_cmp(address, &slot->source_address))
  {
    ++counters.rejected_control_frames;
    return false;
  }
  if ((slot != NULL) && slot->sequence_valid &&
      !Network_SequenceIsNewer(sequence, slot->last_sequence))
  {
    ++counters.rejected_control_frames;
    return false;
  }
  if ((flags & ECU_CONTROL_FLAG_RELEASE) != 0U)
  {
    if (!ECU_DataModelControlIsNeutral(&datagram->control))
    {
      ++counters.rejected_control_frames;
      return false;
    }
    if (slot != NULL)
    {
      slot->active = false;
      slot->sequence_valid = false;
    }
    ++counters.valid_control_frames;
    return true;
  }
  if ((flags & ECU_CONTROL_FLAG_CLEAR_FAULT) != 0U)
  {
    if (!ECU_DataModelControlIsNeutral(&datagram->control) ||
        (SECURE_SafetyClearFault(SAFETY_CLEAR_FAULT_TOKEN) != SAFETY_RESULT_OK))
    {
      ++counters.rejected_control_frames;
      return false;
    }
  }
  if (slot == NULL)
  {
    slot = Network_AcquireSlot(datagram->sender_id, effective_priority);
    if (slot == NULL)
    {
      ++counters.rejected_control_frames;
      return false;
    }
  }

  slot->active = true;
  slot->sequence_valid = true;
  slot->rearm_candidate = rearm_required ? true : false;
  slot->sender_id = datagram->sender_id;
  /* Explicit non-emergency priorities 1..254 arbitrate every sender. A zero
     priority uses the compatibility fallback in Network_EffectivePriority. */
  slot->priority = effective_priority;
  slot->flags = flags & ECU_CONTROL_FLAG_STEERING_RATE;
  slot->last_sequence = sequence;
  slot->last_update_tick = now;
  ip_addr_copy(slot->source_address, *address);
  slot->control = datagram->control;
  ++counters.valid_control_frames;
  return true;
}

static void Network_ReceiveControl(void *argument, struct udp_pcb *pcb,
                                   struct pbuf *packet,
                                   const ip_addr_t *address, u16_t port)
{
  uint8_t frame[ECU_V2_MAX_FRAME_SIZE];
  ECU_V2Header header;
  ECU_ControlDatagramV2 datagram;
  uint32_t now = HAL_GetTick();
  uint16_t frame_length = packet->tot_len;

  (void)argument;
  (void)pcb;
  (void)port;
  if (network_operational == 0U)
  {
    ++counters.rejected_control_frames;
    pbuf_free(packet);
    return;
  }
  if ((frame_length > sizeof(frame)) ||
      (pbuf_copy_partial(packet, frame, frame_length, 0U) != frame_length))
  {
    ++counters.invalid_control_frames;
    pbuf_free(packet);
    return;
  }
  pbuf_free(packet);

  if ((frame_length > 0U) && (frame[0] == ECU_V1_FRAME_START))
  {
    ECU_ControlDatagramV1 legacy;
    bool emergency_requested;

    memset(&legacy, 0, sizeof(legacy));
    if (!ECU_ProtocolDecodeV1(frame, frame_length, &legacy,
                              sizeof(legacy)))
    {
      ++counters.invalid_control_frames;
      ++counters.rejected_control_frames;
      ++counters.legacy_v1_frames_rejected;
      return;
    }

    /* A structurally valid V1 stop is dominant before any actuator-field or
       obsolete-feature validation, matching the V2 fail-safe ordering. */
    emergency_requested = (legacy.control.emergency_stop_on != 0U) ||
                          (legacy.priority == CONTROL_PRIORITY_EMERGENCY);
    memset(&datagram, 0, sizeof(datagram));
    if (emergency_requested)
    {
      datagram.sender_id = legacy.sender_id;
      datagram.priority = legacy.priority;
      datagram.control.emergency_stop_request =
          (legacy.control.emergency_stop_on != 0U) ? 1U : 0U;
    }
    else if (!ECU_ProtocolConvertControlV1(&legacy, &datagram))
    {
      ++counters.rejected_control_frames;
      ++counters.legacy_v1_frames_rejected;
      return;
    }

    if (Network_AcceptControl(&datagram, 0U, legacy.sequence, address, now))
    {
      ++counters.legacy_v1_frames_accepted;
    }
    else
    {
      ++counters.legacy_v1_frames_rejected;
    }
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
  (void)Network_AcceptControl(&datagram, header.flags, header.sequence,
                              address, now);
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
  bool selection_changed;
  bool neutral_disabled;
  int32_t apply_result;

  if (ControlAuthorityPolicy_BeginSafetyRound(
          &authority_policy, now, CONTROL_APPLY_PERIOD_MS))
  {
    /* Expiry is observable even when ethernetif_input() already rebuilt the
       same slot in this loop. Keep authority IDLE and outputs disarmed for one
       complete 20 ms application interval before evaluating the new session. */
    active_slot = NULL;
    active_session_generation = 0U;
    ++counters.authority_switches;
    last_apply_tick = now;
    ECU_DataModelControlLost();
    outputs_armed = 0U;
    return;
  }
  if (ControlAuthorityPolicy_SafetyHoldActive(&authority_policy, now))
  {
    return;
  }

  selection_changed = (selected != active_slot) ||
                      ((selected != NULL) &&
                       (selected->session_generation !=
                        active_session_generation));

  if (selection_changed)
  {
    active_slot = selected;
    active_session_generation = (selected != NULL) ?
        selected->session_generation : 0U;
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

  neutral_disabled = ECU_DataModelControlIsNeutral(&selected->control);
  if (ControlAuthorityPolicy_RearmRequired(&authority_policy,
                                            selected->sender_id) &&
      (!selected->rearm_candidate || !neutral_disabled))
  {
    /* Defense in depth against a corrupted/future receive path. An expired
       session cannot reach Secure with a non-neutral rearm transaction. */
    if (outputs_armed != 0U)
    {
      (void)SECURE_SafetyDisarmOutputs();
      outputs_armed = 0U;
    }
    return;
  }

  if (outputs_armed == 0U)
  {
    if (SECURE_SafetyArmOutputs(SAFETY_ARM_TOKEN) != SAFETY_RESULT_OK)
    {
      return;
    }
    outputs_armed = 1U;
  }
  ++secure_command_sequence;
  apply_result = ECU_DataModelApplyControl(&selected->control, selected->flags,
                                           selected->sender_id,
                                           secure_command_sequence);
  if (apply_result != SAFETY_RESULT_OK)
  {
    (void)SECURE_SafetyDisarmOutputs();
    outputs_armed = 0U;
  }
  else if (selected->rearm_candidate && neutral_disabled &&
           ControlAuthorityPolicy_CompleteNeutralRearm(
               &authority_policy, selected->sender_id, true))
  {
    /* Clearing occurs only after the neutral command was accepted by Secure;
       reception alone is not evidence that the safe state was processed. */
    selected->rearm_candidate = false;
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
  if (frame_length == 0U)
  {
    ++counters.transmit_failures;
    return false;
  }
  packet = pbuf_alloc(PBUF_TRANSPORT, (u16_t)frame_length, PBUF_RAM);
  if (packet == NULL)
  {
    ++counters.transmit_failures;
    return false;
  }
  if (pbuf_take(packet, frame, frame_length) != ERR_OK)
  {
    pbuf_free(packet);
    ++counters.transmit_failures;
    return false;
  }
  result = udp_sendto(pcb, packet, address, port);
  pbuf_free(packet);
  if (result != ERR_OK)
  {
    ++counters.transmit_failures;
  }
  return result == ERR_OK;
}

static bool Network_SendLegacyStatus(void)
{
  ECU_StatusPayloadV1 legacy;
  uint8_t frame[ECU_V1_STATUS_PAYLOAD_SIZE + ECU_V1_FRAME_OVERHEAD];
  size_t frame_length;
  struct pbuf *packet;
  err_t result;

  if (!ECU_ProtocolBuildStatusV1(ECU_DataModelGetStatus(), &legacy))
  {
    ++counters.transmit_failures;
    return false;
  }
  frame_length = ECU_ProtocolEncodeV1(&legacy, sizeof(legacy), frame,
                                      sizeof(frame));
  if (frame_length == 0U)
  {
    ++counters.transmit_failures;
    return false;
  }
  packet = pbuf_alloc(PBUF_TRANSPORT, (u16_t)frame_length, PBUF_RAM);
  if (packet == NULL)
  {
    ++counters.transmit_failures;
    return false;
  }
  if (pbuf_take(packet, frame, frame_length) != ERR_OK)
  {
    pbuf_free(packet);
    ++counters.transmit_failures;
    return false;
  }
  result = udp_sendto(transmit_pcb, packet, &broadcast_address,
                      ECU_STATUS_PORT);
  pbuf_free(packet);
  if (result != ERR_OK)
  {
    ++counters.transmit_failures;
  }
  return result == ERR_OK;
}

static void Network_SendStatus(void)
{
  const ECU_StatusPayloadV2 *status;

  ECU_DataModelUpdateStatus();
  status = ECU_DataModelGetStatus();
  if (Network_SendV2(transmit_pcb, &broadcast_address, ECU_STATUS_PORT,
                     ECU_MESSAGE_STATUS, 0U, status,
                     sizeof(ECU_StatusPayloadV2)))
  {
    ++counters.status_frames_sent;
  }
}

static void Network_SendSteeringStatus(void)
{
  if (!ECU_DataModelUpdateSteeringStatus())
  {
    return;
  }
  if (Network_SendV2(transmit_pcb, &broadcast_address, ECU_STATUS_PORT,
                     ECU_MESSAGE_STEERING_STATUS, 0U,
                     ECU_DataModelGetSteeringStatus(),
                     sizeof(ECU_SteeringStatusPayloadV2)))
  {
    ++counters.steering_status_frames_sent;
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
      ECU_CAP_CAN1_J1939 | ECU_CAP_CAN2_STEERING_RATE | ECU_CAP_SW_I2C_PCB_R1 |
      ECU_CAP_ATECC608_PROBE | ECU_CAP_RELAY_OUTPUTS | ECU_CAP_VALVE_CURRENT_PI |
      ECU_CAP_VALVE_TUNING | ECU_CAP_SIGNED_ETHERNET_OTA |
      ECU_CAP_LEGACY_V1_SAFE_CONTROL | ECU_CAP_LATCHED_ESTOP_RESET;
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

static void Network_SendSecurityStatus(void)
{
  ECU_SecurityPayloadV2 payload = {0};
  SAFETY_SecurityStatus secure_status;

  if (SECURE_SafetyGetSecurityStatus(&secure_status) != SAFETY_RESULT_OK)
  {
    return;
  }
  payload.api_version = secure_status.api_version;
  payload.flags = secure_status.flags;
  payload.atecc_result = secure_status.atecc_result;
  payload.config_crc32c = secure_status.config_crc32c;
  memcpy(payload.mcu_uid, secure_status.mcu_uid, sizeof(payload.mcu_uid));
  memcpy(payload.serial, secure_status.serial, sizeof(payload.serial));
  memcpy(payload.revision, secure_status.revision, sizeof(payload.revision));
  payload.i2c_address = secure_status.i2c_address;
  payload.config_locked = secure_status.config_locked;
  payload.data_locked = secure_status.data_locked;
  payload.device_status = secure_status.device_status;
  payload.auth_result = secure_status.auth_result;
  payload.pairing_generation = secure_status.pairing_generation;
  if (Network_SendV2(transmit_pcb, &broadcast_address,
                     ECU_DIAGNOSTIC_PORT, ECU_MESSAGE_SECURITY_STATUS, 0U,
                     &payload, sizeof(payload)))
  {
    ++counters.security_status_frames_sent;
  }
}

static bool Network_TuningAuthorized(const ip_addr_t *address)
{
  return Network_ServiceHostAuthorized(address) &&
         ((active_slot == NULL) || !active_slot->active ||
          ip_addr_cmp(address, &active_slot->source_address));
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

static bool Network_OtaHostAuthorized(const ip_addr_t *address)
{
  return Network_ServiceHostAuthorized(address);
}

static void Network_SendOtaStatus(const ip_addr_t *address, uint16_t port,
                                  uint32_t request_sequence, int32_t result,
                                  const SAFETY_OtaStatus *status)
{
  ECU_OtaStatusPayload payload = {0};
  payload.result = result;
  payload.request_sequence = request_sequence;
  if (status != NULL) { payload.status = *status; }
  (void)Network_SendV2(ota_pcb, address, port, ECU_MESSAGE_OTA_STATUS, 0U,
                       &payload, sizeof(payload));
}

static void Network_ReceiveOta(void *argument, struct udp_pcb *pcb,
                               struct pbuf *packet,
                               const ip_addr_t *address, u16_t port)
{
  union
  {
    SAFETY_OtaBeginRequest begin;
    SAFETY_OtaChunk chunk;
    uint32_t update_sequence;
  } payload;
  uint8_t frame[ECU_V2_MAX_FRAME_SIZE];
  ECU_V2Header header;
  SAFETY_OtaStatus status = {0};
  uint16_t frame_length = packet->tot_len;
  int32_t result = SAFETY_RESULT_BAD_ARGUMENT;

  (void)argument;
  (void)pcb;
  if (network_operational == 0U)
  {
    pbuf_free(packet);
    return;
  }
  if ((frame_length > sizeof(frame)) ||
      (pbuf_copy_partial(packet, frame, frame_length, 0U) != frame_length))
  {
    pbuf_free(packet);
    return;
  }
  pbuf_free(packet);
  memset(&payload, 0, sizeof(payload));
  if (!Network_OtaHostAuthorized(address) ||
      !ECU_ProtocolDecodeV2(frame, frame_length, &header, &payload,
                            sizeof(payload)))
  {
    return;
  }
  switch (header.message_type)
  {
    case ECU_MESSAGE_OTA_STATUS:
      if (header.payload_size == 0U)
      {
        result = SECURE_SafetyOtaGetStatus(&status);
      }
      break;
    case ECU_MESSAGE_OTA_BEGIN:
      if (header.payload_size == sizeof(payload.begin))
      {
        result = SECURE_SafetyOtaBegin(&payload.begin, &status);
      }
      break;
    case ECU_MESSAGE_OTA_CHUNK:
      if (header.payload_size == sizeof(payload.chunk))
      {
        result = SECURE_SafetyOtaWrite(&payload.chunk, &status);
      }
      break;
    case ECU_MESSAGE_OTA_FINISH:
      if (header.payload_size == sizeof(payload.update_sequence))
      {
        result = SECURE_SafetyOtaFinish(payload.update_sequence, &status);
        if ((result == SAFETY_RESULT_OK) &&
            (status.state == SAFETY_OTA_STATE_READY))
        {
          ota_reset_deadline = HAL_GetTick() + OTA_RESET_DELAY_MS;
          ota_reset_pending = 1U;
        }
      }
      break;
    default:
      result = SAFETY_RESULT_UNSUPPORTED_VERSION;
      break;
  }
  if (status.api_version == 0U)
  {
    (void)SECURE_SafetyOtaGetStatus(&status);
  }
  Network_SendOtaStatus(address, port, header.sequence, result, &status);
}

#if defined(ECU_FACTORY_PROVISIONING)
static bool Network_FactoryHostAuthorized(const ip_addr_t *address)
{
  return Network_ServiceHostAuthorized(address);
}

static void Network_SendFactoryStatus(
    const ip_addr_t *address, uint16_t port, uint32_t request_sequence,
    int32_t result, const SAFETY_FactoryStatus *status)
{
  ECU_FactoryAteccStatusPayload payload = {0};

  payload.result = result;
  payload.request_sequence = request_sequence;
  if (status != NULL)
  {
    payload.phase_flags = status->phase_flags;
    payload.config_crc32c = status->config_crc32c;
    memcpy(payload.mcu_uid, status->mcu_uid, sizeof(payload.mcu_uid));
    payload.slot_locked_mask = status->slot_locked_mask;
    payload.config_locked = status->config_locked;
    payload.data_locked = status->data_locked;
    payload.device_status = status->device_status;
    payload.private_key_slot = status->private_key_slot;
    memcpy(payload.serial, status->serial, sizeof(payload.serial));
    memcpy(payload.revision, status->revision, sizeof(payload.revision));
    memcpy(payload.config, status->config, sizeof(payload.config));
    memcpy(payload.public_key, status->public_key, sizeof(payload.public_key));
  }
  (void)Network_SendV2(tuning_pcb, address, port,
                       ECU_MESSAGE_FACTORY_ATECC_STATUS, 0U, &payload,
                       sizeof(payload));
}
#endif

static void Network_ReceiveTuning(void *argument, struct udp_pcb *pcb,
                                  struct pbuf *packet,
                                  const ip_addr_t *address, u16_t port)
{
  union
  {
    SAFETY_ValveConfig config;
    ECU_TelemetrySubscribePayload subscribe;
#if defined(ECU_FACTORY_PROVISIONING)
    SAFETY_FactoryProvisionRequest factory_request;
#endif
    uint8_t bytes[sizeof(SAFETY_ValveConfig)];
  } payload;
  uint8_t frame[ECU_V2_MAX_FRAME_SIZE];
  ECU_V2Header header;
  uint16_t frame_length = packet->tot_len;
  int32_t result = SAFETY_RESULT_BAD_ARGUMENT;

  (void)argument;
  (void)pcb;
  if (network_operational == 0U)
  {
    pbuf_free(packet);
    return;
  }
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
  /* Do not disclose configuration or emit reflected replies to other hosts.
     Apply/save/reload/subscription retain the active-authority source check. */
  if (!Network_ServiceHostAuthorized(address))
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

    case ECU_MESSAGE_TELEMETRY_UNSUBSCRIBE:
      if ((header.payload_size == 0U) &&
          Network_TuningAuthorized(address) &&
          ((telemetry_client_port == 0U) ||
           ip_addr_cmp(address, &telemetry_client_address)))
      {
        telemetry_client_port = 0U;
        telemetry_subscription_deadline = 0U;
        telemetry_dropped_baseline_valid = 0U;
        result = SAFETY_RESULT_OK;
      }
      else if (!Network_TuningAuthorized(address) ||
               (telemetry_client_port != 0U))
      {
        result = SAFETY_RESULT_CONFLICT;
      }
      Network_SendOperationAck(address, port, header.sequence, result);
      break;

#if defined(ECU_FACTORY_PROVISIONING)
    case ECU_MESSAGE_FACTORY_ATECC_STATUS:
      if ((header.payload_size == 0U) &&
          Network_FactoryHostAuthorized(address))
      {
        SAFETY_FactoryStatus factory_status;
        result = SECURE_SafetyFactoryGetStatus(&factory_status);
        Network_SendFactoryStatus(address, port, header.sequence, result,
                                  &factory_status);
      }
      break;

    case ECU_MESSAGE_FACTORY_ATECC_PROVISION:
      if ((header.payload_size == sizeof(payload.factory_request)) &&
          Network_FactoryHostAuthorized(address))
      {
        SAFETY_FactoryStatus factory_status;
        result = SECURE_SafetyFactoryProvision(&payload.factory_request,
                                                &factory_status);
        Network_SendFactoryStatus(address, port, header.sequence, result,
                                  &factory_status);
      }
      break;
#endif

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

static bool Network_TelemetrySubscriptionActive(uint32_t now)
{
  if (telemetry_client_port == 0U)
  {
    return false;
  }
  if ((int32_t)(now - telemetry_subscription_deadline) < 0)
  {
    return true;
  }

  telemetry_client_port = 0U;
  telemetry_subscription_deadline = 0U;
  telemetry_dropped_baseline_valid = 0U;
  return false;
}

static void Network_ProcessPeriodicTransmit(uint32_t now)
{
  EthernetTxCategory category;
  bool telemetry_active = Network_TelemetrySubscriptionActive(now);

  EthernetTxScheduler_Update(&transmit_scheduler, now, telemetry_active);
  category = EthernetTxScheduler_Pop(&transmit_scheduler, now);
  switch (category)
  {
    case ETHERNET_TX_CATEGORY_STATUS:
      Network_SendStatus();
      break;

    case ETHERNET_TX_CATEGORY_STEERING:
      Network_SendSteeringStatus();
      break;

    case ETHERNET_TX_CATEGORY_DIAGNOSTIC:
      Network_SendDiagnostic();
      break;

    case ETHERNET_TX_CATEGORY_SECURITY:
      Network_SendSecurityStatus();
      break;

    case ETHERNET_TX_CATEGORY_LEGACY:
      if (Network_SendLegacyStatus())
      {
        ++counters.legacy_v1_status_frames_sent;
      }
      break;

    case ETHERNET_TX_CATEGORY_TELEMETRY:
      Network_SendValveTelemetry(now);
      break;

    case ETHERNET_TX_CATEGORY_NONE:
    case ETHERNET_TX_CATEGORY_COUNT:
    default:
      break;
  }
}

bool ECU_NetworkInit(void)
{
  ip4_addr_t ip;
  ip4_addr_t mask;
  ip4_addr_t gateway;
  err_t bind_result;
  uint32_t now;

  memset(&ecu_netif, 0, sizeof(ecu_netif));
  memset(slots, 0, sizeof(slots));
  memset(&counters, 0, sizeof(counters));
  ControlAuthorityPolicy_Init(&authority_policy);
  active_slot = NULL;
  active_session_generation = 0U;
  outputs_armed = 0U;
  secure_command_sequence = 0U;
  telemetry_client_port = 0U;
  telemetry_subscription_deadline = 0U;
  telemetry_dropped_baseline = 0U;
  telemetry_dropped_baseline_valid = 0U;
  transmit_sequence = 0U;
  ota_reset_deadline = 0U;
  ota_reset_pending = 0U;
  network_operational = 0U;

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
  ota_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
  if ((control_pcb == NULL) || (transmit_pcb == NULL) ||
      (tuning_pcb == NULL) || (ota_pcb == NULL))
  {
    if (control_pcb != NULL) { udp_remove(control_pcb); }
    if (transmit_pcb != NULL) { udp_remove(transmit_pcb); }
    if (tuning_pcb != NULL) { udp_remove(tuning_pcb); }
    if (ota_pcb != NULL) { udp_remove(ota_pcb); }
    control_pcb = NULL;
    transmit_pcb = NULL;
    tuning_pcb = NULL;
    ota_pcb = NULL;
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
  if (bind_result == ERR_OK)
  {
    bind_result = udp_bind(ota_pcb, IP_ANY_TYPE, ECU_OTA_PORT);
  }
  if (bind_result != ERR_OK)
  {
    udp_remove(control_pcb);
    udp_remove(transmit_pcb);
    udp_remove(tuning_pcb);
    udp_remove(ota_pcb);
    control_pcb = NULL;
    transmit_pcb = NULL;
    tuning_pcb = NULL;
    ota_pcb = NULL;
    return false;
  }
  udp_recv(control_pcb, Network_ReceiveControl, NULL);
  udp_recv(tuning_pcb, Network_ReceiveTuning, NULL);
  udp_recv(ota_pcb, Network_ReceiveOta, NULL);
  now = HAL_GetTick();
  last_apply_tick = now;
  if (!EthernetTxScheduler_Init(&transmit_scheduler, now,
                                transmit_schedule))
  {
    udp_remove(control_pcb);
    udp_remove(transmit_pcb);
    udp_remove(tuning_pcb);
    udp_remove(ota_pcb);
    control_pcb = NULL;
    transmit_pcb = NULL;
    tuning_pcb = NULL;
    ota_pcb = NULL;
    return false;
  }
  last_link_poll_tick = now;
  return true;
}

void ECU_NetworkSetOperational(bool operational)
{
  network_operational = operational ? 1U : 0U;
  if (network_operational == 0U)
  {
    memset(slots, 0, sizeof(slots));
    ControlAuthorityPolicy_Init(&authority_policy);
    active_slot = NULL;
    active_session_generation = 0U;
    outputs_armed = 0U;
    telemetry_client_port = 0U;
    telemetry_subscription_deadline = 0U;
    telemetry_dropped_baseline_valid = 0U;
    ota_reset_pending = 0U;
    (void)SECURE_SafetyDisarmOutputs();
  }
  else
  {
    uint32_t now = HAL_GetTick();
    last_apply_tick = now;
    if (!EthernetTxScheduler_Init(&transmit_scheduler, now,
                                  transmit_schedule))
    {
      network_operational = 0U;
      (void)SECURE_SafetyDisarmOutputs();
    }
  }
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
  if (network_operational == 0U)
  {
    return;
  }
  Network_ApplyAuthority(now);
  Network_ProcessPeriodicTransmit(now);
  if ((ota_reset_pending != 0U) &&
      ((int32_t)(now - ota_reset_deadline) >= 0))
  {
    (void)SECURE_SafetyDisarmOutputs();
    NVIC_SystemReset();
  }
}

bool ECU_NetworkLinkIsUp(void)
{
  return netif_is_link_up(&ecu_netif);
}

bool ECU_NetworkStartupReady(void)
{
  EthernetPhyHealth health;

  ethernetif_get_phy_health(&health);
  return health.ready != 0U;
}

const ECU_NetworkCounters *ECU_NetworkGetCounters(void)
{
  return &counters;
}
