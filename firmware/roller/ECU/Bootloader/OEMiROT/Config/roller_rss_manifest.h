/**
  ******************************************************************************
  * @file    roller_rss_manifest.h
  * @brief   Pinned RSS service manifest for the Roller ECU boot chain.
  ******************************************************************************
  *
  * The STM32H563 Rev X / SFSP v2.5.0 RSS hand-off masks the API-table words
  * from CPU data reads before OEMiROT starts, while the immutable ROM services
  * remain callable.  Bind the service addresses to all immutable identity
  * values that select this exact ROM build and fail closed on other silicon.
  *
  * Provenance (verified on ECU probe 066BFF565456857187210935):
  *   DBGMCU IDCODE              0x10076484
  *   SFSP version @0x0BF960CC   0x02050000 (v2.5.0)
  *   Descriptor 3 magic         "DSCTR484"
  *   JumpHDPLvl3                0x0FF95079
  *   JumpHDPLvl3NS              0x0FF951C9
  *   Device UID                 00380061-34345112-32383537
  *
  * A later STM32H56x revision may use different entries.  It must receive a
  * separately reviewed manifest; it must never fall through to this one.
  */

#ifndef ROLLER_RSS_MANIFEST_H
#define ROLLER_RSS_MANIFEST_H

#define ROLLER_RSS_DEVICE_ID                 0x484UL
#define ROLLER_RSS_REVISION_ID               0x1007UL
#define ROLLER_RSS_IDCODE                     0x10076484UL
#define ROLLER_RSS_SFSP_VERSION_ADDRESS      0x0BF960CCUL
#define ROLLER_RSS_SFSP_VERSION              0x02050000UL
#define ROLLER_RSS_DESCRIPTOR_INDEX          0UL
#define ROLLER_RSS_DESCRIPTOR_MAGIC0         0x54435344UL /* "DSCT" */
#define ROLLER_RSS_DESCRIPTOR_MAGIC1         0x34383452UL /* "R484" */
#define ROLLER_RSS_JUMP_HDPL3                0x0FF95079UL
#define ROLLER_RSS_JUMP_HDPL3_NS             0x0FF951C9UL

/* Per-device manufacturing identity.  This value is not secret; integrity
 * and non-transferability come from the device-internal HUK-wrapped OEMiROT
 * OBKs, the WRP/HDP-protected pairing record, and the ATECC private key that
 * cannot be exported. */
#define ROLLER_DEVICE_UID0                    0x00380061UL
#define ROLLER_DEVICE_UID1                    0x34345112UL
#define ROLLER_DEVICE_UID2                    0x32383537UL

#endif /* ROLLER_RSS_MANIFEST_H */
