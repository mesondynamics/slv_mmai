#ifndef ECU_DEVICE_PROFILE_H
#define ECU_DEVICE_PROFILE_H

#include "ecu_device_selection.h"

/* Public manufacturing identities, not cryptographic secrets.
 * The serial/IP assignment follows the legacy roller device-ID allocation.
 * UID values must originate from direct Programmer reads of the target MCU.
 * Bootloader, Secure application and network must select the SAME profile.
 */
#if ECU_DEVICE_ID == 1
#define ECU_DEVICE_PRODUCT_SERIAL "SN-EJAHGJI"
#define ECU_DEVICE_IP_OCTET3 11U
#define ECU_DEVICE_DOMAIN_OCTET3 12U
/* Keep the deployed 1.0.20 MAC; do not silently renumber the existing ECU. */
#define ECU_DEVICE_MAC_OCTET5 0x02U
#define ECU_DEVICE_UID0 0x00380061UL
#define ECU_DEVICE_UID1 0x34345112UL
#define ECU_DEVICE_UID2 0x32383537UL
#elif ECU_DEVICE_ID == 2
#define ECU_DEVICE_PRODUCT_SERIAL "SN-EJAHGJQ"
#define ECU_DEVICE_IP_OCTET3 21U
#define ECU_DEVICE_DOMAIN_OCTET3 22U
/* 8a:ea:b5:00:00:02 is already used by SN-EJAHGJI. Reserve .03 here. */
#define ECU_DEVICE_MAC_OCTET5 0x03U
#define ECU_DEVICE_UID0 0x00390044UL
#define ECU_DEVICE_UID1 0x34345112UL
#define ECU_DEVICE_UID2 0x32383537UL
#else
#error "Unreviewed ECU_DEVICE_ID: add and verify a manufacturing profile first"
#endif

#endif
