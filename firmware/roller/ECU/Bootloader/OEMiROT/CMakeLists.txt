cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(STM32_MCU_FLAGS "-mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mcmse")
set(STM32_LINKER_SCRIPT "build/${CMAKE_BUILD_TYPE}/oemirot.ld")
set(STM32_LINKER_OPTION "--specs=nosys.specs")

project(ECU_OEMiROT C ASM)

option(ROLLER_OEMIROT_OPEN_BENCH
       "Accept OPEN or stronger product state without enabling development behavior"
       OFF)

set(STM32CUBE_H5_ROOT
    "/home/plac/STM32Cube/Repository/STM32Cube_FW_H5_V1.7.0"
    CACHE PATH "Path to the exact STM32CubeH5 v1.7.0 package")
set(OEMIROT_EXAMPLE_ROOT
    "${STM32CUBE_H5_ROOT}/Projects/NUCLEO-H563ZI/Applications/ROT/OEMiROT_Boot")
set(MCUBOOT_ROOT "${STM32CUBE_H5_ROOT}/Middlewares/Third_Party/mcuboot")
set(MBEDCRYPTO_ROOT "${STM32CUBE_H5_ROOT}/Middlewares/Third_Party/mbed-crypto")

# The ST reference layout only marks the Secure primary slot as Secure.  This
# product intentionally keeps all of Bank 1 Secure because it also contains the
# Secure secondary image, the encrypted NonSecure secondary image, the ATECC
# pairing identity, the OTA journal, and calibration records.  Generate a
# pinned derivative instead of editing the installed Cube package.  Requiring
# exactly two matches makes a future CubeH5 source change fail closed.
set(OEMIROT_SECURITY_UPSTREAM
    "${OEMIROT_EXAMPLE_ROOT}/Src/low_level_security.c")
file(READ "${OEMIROT_SECURITY_UPSTREAM}" OEMIROT_SECURITY_SOURCE)
set(OEMIROT_SECWM_EXPRESSION
    "end = \\(S_IMAGE_PRIMARY_PARTITION_OFFSET  \\+ FLASH_S_PARTITION_SIZE - 1\\) / PAGE_SIZE")
string(REGEX MATCHALL "${OEMIROT_SECWM_EXPRESSION}"
       OEMIROT_SECWM_MATCHES "${OEMIROT_SECURITY_SOURCE}")
list(LENGTH OEMIROT_SECWM_MATCHES OEMIROT_SECWM_MATCH_COUNT)
if(NOT OEMIROT_SECWM_MATCH_COUNT EQUAL 2)
  message(FATAL_ERROR
    "Pinned CubeH5 low_level_security.c changed: expected two SECWM expressions, found ${OEMIROT_SECWM_MATCH_COUNT}")
endif()
string(REPLACE
  "end = (S_IMAGE_PRIMARY_PARTITION_OFFSET  + FLASH_S_PARTITION_SIZE - 1) / PAGE_SIZE;"
  "end = PAGE_MAX_NUMBER_IN_BANK; /* Roller ECU: all of Bank 1 is Secure. */"
  OEMIROT_SECURITY_SOURCE "${OEMIROT_SECURITY_SOURCE}")

# The RSS/SFSP hand-off leaves RAMCFG_IRQn pending on this STM32H563 Rev X.
# The ST reference enables the line without clearing that inherited NVIC state,
# which immediately enters the weak default handler.  Patch the pinned source
# derivative and fail the configure if a future CubeH5 update changes the site.
set(OEMIROT_RAMCFG_ENABLE_LINE
    "    HAL_NVIC_EnableIRQ(RAMCFG_IRQn);")
string(REGEX MATCHALL "HAL_NVIC_EnableIRQ\\(RAMCFG_IRQn\\)"
       OEMIROT_RAMCFG_ENABLE_MATCHES "${OEMIROT_SECURITY_SOURCE}")
list(LENGTH OEMIROT_RAMCFG_ENABLE_MATCHES OEMIROT_RAMCFG_ENABLE_MATCH_COUNT)
if(NOT OEMIROT_RAMCFG_ENABLE_MATCH_COUNT EQUAL 1)
  message(FATAL_ERROR
    "Pinned CubeH5 low_level_security.c changed: expected one RAMCFG IRQ enable, found ${OEMIROT_RAMCFG_ENABLE_MATCH_COUNT}")
endif()
string(REPLACE
  "${OEMIROT_RAMCFG_ENABLE_LINE}"
  "    /* Roller ECU: discard RSS/SFSP pending state before unmasking ECC. */\n    HAL_NVIC_ClearPendingIRQ(RAMCFG_IRQn);\n${OEMIROT_RAMCFG_ENABLE_LINE}"
  OEMIROT_SECURITY_SOURCE "${OEMIROT_SECURITY_SOURCE}")
set(OEMIROT_SECURITY_DERIVATIVE
    "${CMAKE_CURRENT_BINARY_DIR}/low_level_security_roller_ecu.c")
file(WRITE "${OEMIROT_SECURITY_DERIVATIVE}" "${OEMIROT_SECURITY_SOURCE}")

# STM32CubeH5 v1.7.0 resolves the RSS descriptor only after its reset-time
# peripheral setup, C runtime, SAU and MPU policy.  On this STM32H563 Rev X the
# RSS function table is already hidden from the Secure CPU by then, even though
# it is valid through the debug access port.  Board characterization confirmed
# that the Secure CPU sees zeros for the SFSP version and descriptor aperture;
# the product therefore uses a reviewed manufacturing manifest rather than a
# live read of that aperture.  Generate product derivatives which:
#   * preserve ST's register-only Reset_Handler sequence without adding a C
#     call before the source's explicit "stack may be used" boundary;
#   * in every lifecycle profile, bind the reviewed silicon ID without reading
#     DBGMCU because debug/trace clocks are not an autonomous-boot contract;
#   * validate both pinned RSS entries and check their Thumb/range properties;
#   * store each identity, entry and selector with complements in a NOLOAD section of
#     ECC-protected Secure RAM, so the C runtime cannot clear the snapshot;
#   * publish the reviewed per-device manufacturing identity without touching
#     the H563 engineering-information aperture at OEMiROT HDPL1;
#   * never silently falls back to an unvalidated descriptor.
set(OEMIROT_BOOT_HAL_UPSTREAM
    "${OEMIROT_EXAMPLE_ROOT}/Src/boot_hal.c")
file(READ "${OEMIROT_BOOT_HAL_UPSTREAM}" OEMIROT_BOOT_HAL_SOURCE)

set(OEMIROT_BOOT_INCLUDE_OLD [=[
#include "boot_hal_cfg.h"
]=])
set(OEMIROT_BOOT_INCLUDE_NEW [=[
#include "boot_hal_cfg.h"
#include "roller_rss_manifest.h"
#include "ecu_boot_handoff.h"
]=])
string(FIND "${OEMIROT_BOOT_HAL_SOURCE}" "${OEMIROT_BOOT_INCLUDE_OLD}"
       OEMIROT_BOOT_INCLUDE_POSITION)
if(OEMIROT_BOOT_INCLUDE_POSITION EQUAL -1)
  message(FATAL_ERROR "Pinned CubeH5 boot_hal.c changed: boot HAL include was not found")
endif()
string(REPLACE "${OEMIROT_BOOT_INCLUDE_OLD}" "${OEMIROT_BOOT_INCLUDE_NEW}"
       OEMIROT_BOOT_HAL_SOURCE "${OEMIROT_BOOT_HAL_SOURCE}")

set(OEMIROT_BOOT_GLOBALS_OLD [=[
STM32xx_Descriptor_t *DescriptorBase = (STM32xx_Descriptor_t *)STM32_DESCRIPTOR_BASE_NS_3;
RSSLIB_pFunc_TypeDef *Rss_lib_p = (RSSLIB_pFunc_TypeDef *)RSSLIB_PFUNC_3;
]=])
set(OEMIROT_BOOT_GLOBALS_NEW [=[
STM32xx_Descriptor_t *DescriptorBase = (STM32xx_Descriptor_t *)STM32_DESCRIPTOR_BASE_NS_3;
RSSLIB_pFunc_TypeDef *Rss_lib_p = (RSSLIB_pFunc_TypeDef *)RSSLIB_PFUNC_3;

/* Resolved before the descriptor window becomes inaccessible at HDPL1. */
typedef struct
{
  uint32_t marker;
  uint32_t marker_inv;
  uint32_t descriptor_index;
  uint32_t descriptor_index_inv;
  uint32_t idcode;
  uint32_t idcode_inv;
  uint32_t sfsp_version;
  uint32_t sfsp_version_inv;
  uint32_t magic0;
  uint32_t magic0_inv;
  uint32_t magic1;
  uint32_t magic1_inv;
  uint32_t hdp3;
  uint32_t hdp3_inv;
  uint32_t hdp3ns;
  uint32_t hdp3ns_inv;
} RollerRssCache_t;

static volatile RollerRssCache_t RollerRssCache
  __attribute__((section(".bss.NoInit.roller_rss"), aligned(8), used));
]=])
string(FIND "${OEMIROT_BOOT_HAL_SOURCE}" "${OEMIROT_BOOT_GLOBALS_OLD}"
       OEMIROT_BOOT_GLOBALS_POSITION)
if(OEMIROT_BOOT_GLOBALS_POSITION EQUAL -1)
  message(FATAL_ERROR "Pinned CubeH5 boot_hal.c changed: RSS globals were not found")
endif()
string(REPLACE "${OEMIROT_BOOT_GLOBALS_OLD}" "${OEMIROT_BOOT_GLOBALS_NEW}"
       OEMIROT_BOOT_HAL_SOURCE "${OEMIROT_BOOT_HAL_SOURCE}")

set(OEMIROT_BOOT_PROTOTYPES_OLD [=[
void icache_init(void);
void getDescriptorAdd(void);
]=])
set(OEMIROT_BOOT_PROTOTYPES_NEW [=[
void icache_init(void);
void getDescriptorAdd(void);
static void Roller_ManufacturingManifestInit(void);
static uint32_t Roller_GetCachedRssService(uint32_t service, uint32_t service_inv);
]=])
string(FIND "${OEMIROT_BOOT_HAL_SOURCE}" "${OEMIROT_BOOT_PROTOTYPES_OLD}"
       OEMIROT_BOOT_PROTOTYPES_POSITION)
if(OEMIROT_BOOT_PROTOTYPES_POSITION EQUAL -1)
  message(FATAL_ERROR "Pinned CubeH5 boot_hal.c changed: RSS prototypes were not found")
endif()
string(REPLACE "${OEMIROT_BOOT_PROTOTYPES_OLD}" "${OEMIROT_BOOT_PROTOTYPES_NEW}"
       OEMIROT_BOOT_HAL_SOURCE "${OEMIROT_BOOT_HAL_SOURCE}")

string(REGEX MATCHALL
       "rsslib_sec_jump_HDP_lvl3 = \\(uint32_t\\)\\(Rss_lib_p->S.JumpHDPLvl3\\)"
       OEMIROT_HDP3_LOAD_MATCHES "${OEMIROT_BOOT_HAL_SOURCE}")
list(LENGTH OEMIROT_HDP3_LOAD_MATCHES OEMIROT_HDP3_LOAD_MATCH_COUNT)
if(NOT OEMIROT_HDP3_LOAD_MATCH_COUNT EQUAL 2)
  message(FATAL_ERROR
    "Pinned CubeH5 boot_hal.c changed: expected two HDPL3 entry loads, found ${OEMIROT_HDP3_LOAD_MATCH_COUNT}")
endif()
string(REPLACE
  "rsslib_sec_jump_HDP_lvl3 = (uint32_t)(Rss_lib_p->S.JumpHDPLvl3);"
  "rsslib_sec_jump_HDP_lvl3 = Roller_GetCachedRssService(RollerRssCache.hdp3, RollerRssCache.hdp3_inv);"
  OEMIROT_BOOT_HAL_SOURCE "${OEMIROT_BOOT_HAL_SOURCE}")

string(REGEX MATCHALL
       "rsslib_sec_jump_HDP_lvl3ns = \\(uint32_t\\)\\(Rss_lib_p->S.JumpHDPLvl3NS\\)"
       OEMIROT_HDP3NS_LOAD_MATCHES "${OEMIROT_BOOT_HAL_SOURCE}")
list(LENGTH OEMIROT_HDP3NS_LOAD_MATCHES OEMIROT_HDP3NS_LOAD_MATCH_COUNT)
if(NOT OEMIROT_HDP3NS_LOAD_MATCH_COUNT EQUAL 1)
  message(FATAL_ERROR
    "Pinned CubeH5 boot_hal.c changed: expected one HDPL3NS entry load, found ${OEMIROT_HDP3NS_LOAD_MATCH_COUNT}")
endif()
string(REPLACE
  "rsslib_sec_jump_HDP_lvl3ns = (uint32_t)(Rss_lib_p->S.JumpHDPLvl3NS);"
  "rsslib_sec_jump_HDP_lvl3ns = Roller_GetCachedRssService(RollerRssCache.hdp3ns, RollerRssCache.hdp3ns_inv);"
  OEMIROT_BOOT_HAL_SOURCE "${OEMIROT_BOOT_HAL_SOURCE}")

set(OEMIROT_DESCRIPTOR_FUNCTION_OLD [=[
void getDescriptorAdd(void)
{
    char ReadMagic[9] = {0};
    for(int i=0; i<8; i++)
    {
      ReadMagic[i] = DescriptorBase->Magic[i];
    }
    if(strcmp(ReadMagic, "DSCTR484") != 0)
    {
      DescriptorBase = (STM32xx_Descriptor_t *)STM32_DESCRIPTOR_BASE_NS_2;
      Rss_lib_p = (RSSLIB_pFunc_TypeDef *)RSSLIB_PFUNC_2;
      for(int i=0; i<8; i++)
      {
        ReadMagic[i] = DescriptorBase->Magic[i];
      }
      if(strcmp(ReadMagic, "DSCTR484") != 0)
      {
        DescriptorBase = (STM32xx_Descriptor_t *)STM32_DESCRIPTOR_BASE_NS_1;
        Rss_lib_p = (RSSLIB_pFunc_TypeDef *)RSSLIB_PFUNC_1;
      }
    }
}
]=])
set(OEMIROT_DESCRIPTOR_FUNCTION_NEW [=[
static uint32_t Roller_GetCachedRssService(uint32_t service, uint32_t service_inv)
{
    uint32_t target = service & ~1UL;

    if (((service ^ service_inv) != UINT32_MAX) ||
        ((service & 1UL) == 0UL) ||
        (target < RSS_LIB_BASE) ||
        (target >= (RSS_LIB_BASE + RSS_LIB_SIZE)))
    {
      Error_Handler();
    }
    return service;
}

#define ROLLER_RSS_CACHE_MARKER 0x52535343UL /* "RSSC" */

static uint32_t Roller_BootHandoffCrc32c(
    const volatile ECU_BootHandoff *handoff)
{
    const volatile uint8_t *bytes =
        (const volatile uint8_t *)&handoff->schema;
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t byte_index;
    uint32_t bit;

    for (byte_index = 0UL;
         byte_index < (offsetof(ECU_BootHandoff, crc32c) -
                       offsetof(ECU_BootHandoff, schema));
         ++byte_index)
    {
      crc ^= bytes[byte_index];
      for (bit = 0UL; bit < 8UL; ++bit)
      {
        crc = (crc >> 1U) ^ ((crc & 1UL) ? 0x82F63B78UL : 0UL);
      }
    }
    return crc ^ 0xFFFFFFFFUL;
}

static __attribute__((noinline)) void Roller_ManufacturingManifestInit(void)
{
    volatile ECU_BootHandoff *handoff =
        (volatile ECU_BootHandoff *)(uintptr_t)ECU_BOOT_HANDOFF_ADDRESS;
    uint32_t idcode_a;
    uint32_t index;

    /* C runtime and the ECC SRAM startup sequence are complete before this
       function runs.  Rebuild both records from reviewed manufacturing data.
       Do not read UID_BASE here: on STM32H563 Rev X the HDPL1 Secure CPU
       receives zero (or a fault, depending on inherited SAU/cache state) even
       though the debug access port can report the UID. */
    RollerRssCache.marker = 0UL;
    RollerRssCache.marker_inv = 0UL;
    handoff->magic = 0UL;
    handoff->magic_inv = 0UL;
    __DSB();

    /*
     * The SFSP/descriptor aperture is not CPU-readable after the RSS handoff
     * on STM32H563 Rev X.  The matching immutable SFSP/descriptor values were
     * captured via the debug access port during provisioning and are pinned by
     * the reviewed roller_rss_manifest.h.  Both OPEN and CLOSED use that exact
     * reviewed manifest without touching DBGMCU.  The per-device HUK-wrapped
     * OBKeys and ATECC pairing provide the runtime anti-transplant binding.
     */
    idcode_a = ROLLER_RSS_IDCODE;

    /*
     * The UID is a reviewed manufacturing input to this per-device OEMiROT
     * build.  Runtime anti-transplant protection does not rely on secrecy of
     * these words: the paired ATECC public identity record resides outside
     * signed OTA images in CLOSED/HDP-protected Secure flash, while the ATECC
     * private key is non-exportable.  A replacement MCU has neither record
     * and therefore cannot pass the startup challenge.
     */
    handoff->schema = ECU_BOOT_HANDOFF_SCHEMA;
    handoff->schema_inv = ~ECU_BOOT_HANDOFF_SCHEMA;
    handoff->record_size = sizeof(ECU_BootHandoff);
    handoff->record_size_inv = ~(uint32_t)sizeof(ECU_BootHandoff);
    handoff->idcode = idcode_a;
    handoff->idcode_inv = ~idcode_a;
    handoff->mcu_uid[0] = ROLLER_DEVICE_UID0;
    handoff->mcu_uid[1] = ROLLER_DEVICE_UID1;
    handoff->mcu_uid[2] = ROLLER_DEVICE_UID2;
    for (index = 0UL; index < 3UL; ++index)
    {
      handoff->mcu_uid_inv[index] = ~handoff->mcu_uid[index];
    }
    handoff->crc32c = Roller_BootHandoffCrc32c(handoff);
    handoff->crc32c_inv = ~handoff->crc32c;
    handoff->magic_inv = ~ECU_BOOT_HANDOFF_MAGIC;

    RollerRssCache.descriptor_index = ROLLER_RSS_DESCRIPTOR_INDEX;
    RollerRssCache.descriptor_index_inv = ~ROLLER_RSS_DESCRIPTOR_INDEX;
    RollerRssCache.idcode = idcode_a;
    RollerRssCache.idcode_inv = ~idcode_a;
    RollerRssCache.sfsp_version = ROLLER_RSS_SFSP_VERSION;
    RollerRssCache.sfsp_version_inv = ~ROLLER_RSS_SFSP_VERSION;
    RollerRssCache.magic0 = ROLLER_RSS_DESCRIPTOR_MAGIC0;
    RollerRssCache.magic0_inv = ~ROLLER_RSS_DESCRIPTOR_MAGIC0;
    RollerRssCache.magic1 = ROLLER_RSS_DESCRIPTOR_MAGIC1;
    RollerRssCache.magic1_inv = ~ROLLER_RSS_DESCRIPTOR_MAGIC1;
    RollerRssCache.hdp3 = ROLLER_RSS_JUMP_HDPL3;
    RollerRssCache.hdp3_inv = ~ROLLER_RSS_JUMP_HDPL3;
    RollerRssCache.hdp3ns = ROLLER_RSS_JUMP_HDPL3_NS;
    RollerRssCache.hdp3ns_inv = ~ROLLER_RSS_JUMP_HDPL3_NS;
    RollerRssCache.marker_inv = ~ROLLER_RSS_CACHE_MARKER;
    __DSB();
    RollerRssCache.marker = ROLLER_RSS_CACHE_MARKER;
    handoff->magic = ECU_BOOT_HANDOFF_MAGIC;
    __DSB();
}

void getDescriptorAdd(void)
{
    static const uint32_t descriptor_addresses[3] =
    {
      STM32_DESCRIPTOR_BASE_NS_3,
      STM32_DESCRIPTOR_BASE_NS_2,
      STM32_DESCRIPTOR_BASE_NS_1
    };
    static const uint32_t function_table_addresses[3] =
    {
      RSSLIB_PFUNC_3,
      RSSLIB_PFUNC_2,
      RSSLIB_PFUNC_1
    };
    uint32_t index = RollerRssCache.descriptor_index;

    if ((RollerRssCache.marker != ROLLER_RSS_CACHE_MARKER) ||
        ((RollerRssCache.marker ^ RollerRssCache.marker_inv) != UINT32_MAX) ||
        ((index ^ RollerRssCache.descriptor_index_inv) != UINT32_MAX) ||
        (index != ROLLER_RSS_DESCRIPTOR_INDEX) ||
        ((RollerRssCache.idcode ^ RollerRssCache.idcode_inv) != UINT32_MAX) ||
        (RollerRssCache.idcode != ROLLER_RSS_IDCODE) ||
        ((RollerRssCache.sfsp_version ^ RollerRssCache.sfsp_version_inv) !=
         UINT32_MAX) ||
        (RollerRssCache.sfsp_version != ROLLER_RSS_SFSP_VERSION) ||
        ((RollerRssCache.magic0 ^ RollerRssCache.magic0_inv) != UINT32_MAX) ||
        (RollerRssCache.magic0 != ROLLER_RSS_DESCRIPTOR_MAGIC0) ||
        ((RollerRssCache.magic1 ^ RollerRssCache.magic1_inv) != UINT32_MAX) ||
        (RollerRssCache.magic1 != ROLLER_RSS_DESCRIPTOR_MAGIC1) ||
        (RollerRssCache.hdp3 != ROLLER_RSS_JUMP_HDPL3) ||
        (RollerRssCache.hdp3ns != ROLLER_RSS_JUMP_HDPL3_NS))
    {
      Error_Handler();
    }

    (void)Roller_GetCachedRssService(RollerRssCache.hdp3,
                                     RollerRssCache.hdp3_inv);
    (void)Roller_GetCachedRssService(RollerRssCache.hdp3ns,
                                     RollerRssCache.hdp3ns_inv);
    DescriptorBase = (STM32xx_Descriptor_t *)descriptor_addresses[index];
    Rss_lib_p = (RSSLIB_pFunc_TypeDef *)function_table_addresses[index];
    __DSB();
}
]=])
string(FIND "${OEMIROT_BOOT_HAL_SOURCE}" "${OEMIROT_DESCRIPTOR_FUNCTION_OLD}"
       OEMIROT_DESCRIPTOR_FUNCTION_POSITION)
if(OEMIROT_DESCRIPTOR_FUNCTION_POSITION EQUAL -1)
  message(FATAL_ERROR "Pinned CubeH5 boot_hal.c changed: descriptor resolver was not found")
endif()
string(REPLACE "${OEMIROT_DESCRIPTOR_FUNCTION_OLD}" "${OEMIROT_DESCRIPTOR_FUNCTION_NEW}"
       OEMIROT_BOOT_HAL_SOURCE "${OEMIROT_BOOT_HAL_SOURCE}")

set(OEMIROT_PLATFORM_ORDER_OLD [=[
    /* Apply Run time Protection */
    LL_SECU_ApplyRunTimeProtections();
    /* Check static protections */
    LL_SECU_CheckStaticProtections();

    /* get descriptor address to support new and old descriptor */
    getDescriptorAdd();
]=])
set(OEMIROT_PLATFORM_ORDER_NEW [=[
    /* Apply Run time Protection */
    LL_SECU_ApplyRunTimeProtections();
    /* Check static protections */
    LL_SECU_CheckStaticProtections();
]=])
string(FIND "${OEMIROT_BOOT_HAL_SOURCE}" "${OEMIROT_PLATFORM_ORDER_OLD}"
       OEMIROT_PLATFORM_ORDER_POSITION)
if(OEMIROT_PLATFORM_ORDER_POSITION EQUAL -1)
  message(FATAL_ERROR "Pinned CubeH5 boot_hal.c changed: platform protection order was not found")
endif()
string(REPLACE "${OEMIROT_PLATFORM_ORDER_OLD}" "${OEMIROT_PLATFORM_ORDER_NEW}"
       OEMIROT_BOOT_HAL_SOURCE "${OEMIROT_BOOT_HAL_SOURCE}")

set(OEMIROT_PLATFORM_ENTRY_OLD [=[
int32_t boot_platform_init(void)
{
#if defined(MCUBOOT_EXT_LOADER)
    GPIO_InitTypeDef GPIO_Init;
#endif /* MCUBOOT_EXT_LOADER */
]=])
set(OEMIROT_PLATFORM_ENTRY_NEW [=[
int32_t boot_platform_init(void)
{
#if defined(MCUBOOT_EXT_LOADER)
    GPIO_InitTypeDef GPIO_Init;
#endif /* MCUBOOT_EXT_LOADER */

    /* ST's Reset_Handler permits general C/stack use only after its ECC SRAM
       startup boundary.  Initialize the reviewed manifest after C runtime,
       then resolve it before applying platform protections. */
    Roller_ManufacturingManifestInit();
    getDescriptorAdd();
]=])
string(FIND "${OEMIROT_BOOT_HAL_SOURCE}" "${OEMIROT_PLATFORM_ENTRY_OLD}"
       OEMIROT_PLATFORM_ENTRY_POSITION)
if(OEMIROT_PLATFORM_ENTRY_POSITION EQUAL -1)
  message(FATAL_ERROR "Pinned CubeH5 boot_hal.c changed: platform entry was not found")
endif()
string(REPLACE "${OEMIROT_PLATFORM_ENTRY_OLD}" "${OEMIROT_PLATFORM_ENTRY_NEW}"
       OEMIROT_BOOT_HAL_SOURCE "${OEMIROT_BOOT_HAL_SOURCE}")

set(OEMIROT_BOOT_HAL_DERIVATIVE
    "${CMAKE_CURRENT_BINARY_DIR}/boot_hal_roller_ecu.c")
file(WRITE "${OEMIROT_BOOT_HAL_DERIVATIVE}" "${OEMIROT_BOOT_HAL_SOURCE}")

# Keep ST's pre-RAM Reset_Handler free of project-added C calls.  The separate
# pinned derivative removes debug-domain timing from every lifecycle profile;
# the manufacturing manifest is initialized later from boot_platform_init(),
# after ECC SRAM and C runtime are ready.  This keeps the installed STM32Cube
# package immutable and makes an upstream Reset_Handler change a configure-time
# failure.
set(OEMIROT_STARTUP_UPSTREAM
    "${OEMIROT_EXAMPLE_ROOT}/Src/startup_stm32h5xx.c")
file(READ "${OEMIROT_STARTUP_UPSTREAM}" OEMIROT_STARTUP_SOURCE)

set(OEMIROT_RESET_ENTRY_OLD [=[
void Reset_Handler(void)
{
  /* Configure DWT to enable cycles counter */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk | CoreDebug_DEMCR_MON_EN_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
]=])
set(OEMIROT_RESET_ENTRY_NEW [=[
void Reset_Handler(void)
{
]=])
string(FIND "${OEMIROT_STARTUP_SOURCE}" "${OEMIROT_RESET_ENTRY_OLD}"
       OEMIROT_RESET_ENTRY_POSITION)
if(OEMIROT_RESET_ENTRY_POSITION EQUAL -1)
  message(FATAL_ERROR "Pinned CubeH5 startup changed: Reset_Handler entry was not found")
endif()
string(REPLACE "${OEMIROT_RESET_ENTRY_OLD}" "${OEMIROT_RESET_ENTRY_NEW}"
       OEMIROT_STARTUP_SOURCE "${OEMIROT_STARTUP_SOURCE}")
set(OEMIROT_STARTUP_DERIVATIVE
    "${CMAKE_CURRENT_BINARY_DIR}/startup_stm32h5xx_roller_ecu.c")
file(WRITE "${OEMIROT_STARTUP_DERIVATIVE}" "${OEMIROT_STARTUP_SOURCE}")

# Upstream MCUboot treats each image_ok byte and hardware rollback counter
# independently. Roller ECU releases Secure and NonSecure as one dependency-
# locked safety unit, so a power loss between those two Flash program units
# must retain paired rollback. Generate a pinned loader derivative which:
#   * converts a coherent NONE/REVERT mixed state into REVERT/REVERT;
#   * identifies a release by both its version and signed security counter;
#   * advances neither anti-rollback counter until both image_ok flags are SET;
#   * rejects PERM and conflicting TEST/REVERT states.
set(OEMIROT_LOADER_UPSTREAM
    "${MCUBOOT_ROOT}/boot/bootutil/src/loader.c")
file(READ "${OEMIROT_LOADER_UPSTREAM}" OEMIROT_LOADER_SOURCE)

set(OEMIROT_LOADER_INCLUDE_OLD [=[
#include "mcuboot_config/mcuboot_config.h"
]=])
set(OEMIROT_LOADER_INCLUDE_NEW [=[
#include "mcuboot_config/mcuboot_config.h"
#include "roller_pair_policy.h"
]=])
string(FIND "${OEMIROT_LOADER_SOURCE}" "${OEMIROT_LOADER_INCLUDE_OLD}"
       OEMIROT_LOADER_INCLUDE_POSITION)
if(OEMIROT_LOADER_INCLUDE_POSITION EQUAL -1)
  message(FATAL_ERROR
    "Pinned MCUboot loader changed: configuration include was not found")
endif()
string(REPLACE "${OEMIROT_LOADER_INCLUDE_OLD}" "${OEMIROT_LOADER_INCLUDE_NEW}"
       OEMIROT_LOADER_SOURCE "${OEMIROT_LOADER_SOURCE}")

# A partial swap is resumed inside boot_prepare_image_for_update(), before the
# clean-swap policy below can inspect state->swap_type.  Reject an interrupted
# PERM operation before review/completion can make any further Flash write and
# erase the evidence by converting the state to NONE.
set(OEMIROT_LOADER_PARTIAL_PERM_OLD [=[
        if (!boot_status_is_reset(bs)) {

#if (BOOT_IMAGE_NUMBER > 1) && !defined(MCUBOOT_PRIMARY_ONLY)
            boot_review_image_swap_types(state, true);
]=])
set(OEMIROT_LOADER_PARTIAL_PERM_NEW [=[
        if (!boot_status_is_reset(bs)) {

#if (BOOT_IMAGE_NUMBER == 2) && !defined(MCUBOOT_PRIMARY_ONLY)
            if (RollerPair_PartialSwapIsForbidden(
                    bs->swap_type, BOOT_SWAP_TYPE_PERM)) {
                BOOT_LOG_ERR("Interrupted permanent swap is forbidden");
                BOOT_SWAP_TYPE(state) = BOOT_SWAP_TYPE_PANIC;
                FIH_PANIC;
                return;
            }
#endif

#if (BOOT_IMAGE_NUMBER > 1) && !defined(MCUBOOT_PRIMARY_ONLY)
            boot_review_image_swap_types(state, true);
]=])
string(FIND "${OEMIROT_LOADER_SOURCE}"
       "${OEMIROT_LOADER_PARTIAL_PERM_OLD}"
       OEMIROT_LOADER_PARTIAL_PERM_POSITION)
if(OEMIROT_LOADER_PARTIAL_PERM_POSITION EQUAL -1)
  message(FATAL_ERROR
    "Pinned MCUboot loader changed: partial-swap recovery entry was not found")
endif()
string(REPLACE "${OEMIROT_LOADER_PARTIAL_PERM_OLD}"
       "${OEMIROT_LOADER_PARTIAL_PERM_NEW}"
       OEMIROT_LOADER_SOURCE "${OEMIROT_LOADER_SOURCE}")

set(OEMIROT_LOADER_CONFIRM_OLD [=[
#if defined(MCUBOOT_HW_ROLLBACK_PROT) && !defined(MCUBOOT_OVERWRITE_ONLY)
    /* Iterate over all the images. At this point there are no aborted swaps
     * and the swap types are determined for each image. By the end of the loop,
     * the security counters will be updated.
     * If a security counter update is performed, the system must reset,
     * because swap type has to be re-evaluated.
     */
    IMAGES_ITER(BOOT_CURR_IMG(state)) {
        /* Update the stored security counter with the active image's security
         * counter value. It will only be updated if the new security counter is
         * greater than the stored value, and if the active image is confirmed:
         * image has marked itself "OK" (the image_ok flag has been set).
         * This way a "revert" can be performed when it's necessary.
         */
        /* Get active image trailer state: primary slot confirmed? */
        rc = boot_read_swap_state_by_id(FLASH_AREA_IMAGE_PRIMARY(BOOT_CURR_IMG(state)),
                                        &swap_state);
        assert(rc == 0);

        /* Check if primary slot contains a confirmed image */
        if ((swap_state.image_ok != BOOT_FLAG_UNSET) &&
            (boot_is_header_valid(boot_img_hdr(state, BOOT_PRIMARY_SLOT),
                                  BOOT_IMG_AREA(state, BOOT_PRIMARY_SLOT)))) {
            /* Image already confirmed. */
            rc = boot_update_security_counter(
                                    BOOT_CURR_IMG(state),
                                    BOOT_PRIMARY_SLOT,
                                    boot_img_hdr(state, BOOT_PRIMARY_SLOT),
                                    &security_counter_updated);
            if (rc != 0) {
                BOOT_LOG_ERR("Security counter update failed after image "
                             "validation.");
                goto out;
            }

            /* If security cnt has changed, then reset to start again mcuboot
             * process from start with up to date NVCounters */
            if (security_counter_updated) {
                BOOT_LOG_INF("Security counter updated. Reset device");
                NVIC_SystemReset();
            }

        }
    }
#endif /* MCUBOOT_HW_ROLLBACK_PROT && !MCUBOOT_OVERWRITE_ONLY */
]=])
set(OEMIROT_LOADER_CONFIRM_NEW [=[
#if (BOOT_IMAGE_NUMBER == 2) && !defined(MCUBOOT_PRIMARY_ONLY)
    bool roller_primary_pair_coherent;
    bool roller_pair_requires_revert;
    bool roller_target_pair_required;
    fih_int roller_primary_pair_preconfirmed = FIH_FAILURE;
    bool roller_final_primary_verified[BOOT_IMAGE_NUMBER] = { false, false };
    RollerPair_ReleaseIdentity roller_primary_identity[BOOT_IMAGE_NUMBER];
    RollerPair_ReleaseIdentity roller_target_identity[BOOT_IMAGE_NUMBER];
    uint8_t roller_target_slot[BOOT_IMAGE_NUMBER];

    /* This product accepts TEST swaps only. A PERM trailer would independently
     * advance one counter inside upstream boot_perform_update(), violating the
     * paired commit contract. */
    for (image_index = 0; image_index < BOOT_IMAGE_NUMBER; ++image_index) {
        if (state->swap_type[image_index] == BOOT_SWAP_TYPE_PERM) {
            BOOT_LOG_ERR("Permanent swap is forbidden for paired releases");
            rc = BOOT_EBADIMAGE;
            goto out;
        }
    }
    if (RollerPair_SwapTypesConflict(state->swap_type,
                                     BOOT_IMAGE_NUMBER,
                                     BOOT_SWAP_TYPE_TEST,
                                     BOOT_SWAP_TYPE_REVERT)) {
        BOOT_LOG_ERR("Conflicting paired TEST/REVERT state");
        rc = BOOT_EBADIMAGE;
        goto out;
    }

    /* The application confirms two trailers in separate Flash program units.
     * A NONE/REVERT combination is a half-confirmed pair only while both
     * primary slots carry the same signed release identity. Once one side of
     * a paired revert has completed, the primary identities differ; leaving
     * the state untouched lets MCUboot finish only the remaining side and
     * prevents a reverse mixed pair. Both version and protected security
     * counter are required so a reused version cannot alias another release. */
    roller_pair_requires_revert =
        RollerPair_SwapTypesRequireRevert(state->swap_type,
                                          BOOT_IMAGE_NUMBER,
                                          BOOT_SWAP_TYPE_NONE,
                                          BOOT_SWAP_TYPE_REVERT);
    roller_primary_pair_coherent = false;
    if (roller_pair_requires_revert) {
        for (image_index = 0; image_index < BOOT_IMAGE_NUMBER; ++image_index) {
            rc = bootutil_get_img_security_cnt(
                    &state->imgs[image_index][BOOT_PRIMARY_SLOT].hdr,
                    state->imgs[image_index][BOOT_PRIMARY_SLOT].area,
                    &roller_primary_identity[image_index].security_counter);
            if (rc != 0) {
                BOOT_LOG_ERR("Paired primary release identity is invalid");
                goto out;
            }
            roller_primary_identity[image_index].major =
                state->imgs[image_index][BOOT_PRIMARY_SLOT].hdr.ih_ver.iv_major;
            roller_primary_identity[image_index].minor =
                state->imgs[image_index][BOOT_PRIMARY_SLOT].hdr.ih_ver.iv_minor;
            roller_primary_identity[image_index].revision =
                state->imgs[image_index][BOOT_PRIMARY_SLOT].hdr.ih_ver.iv_revision;
            roller_primary_identity[image_index].build =
                state->imgs[image_index][BOOT_PRIMARY_SLOT].hdr.ih_ver.iv_build_num;
        }
        roller_primary_pair_coherent =
            RollerPair_ReleaseIdentitiesMatch(&roller_primary_identity[0],
                                              &roller_primary_identity[1]);
    }
    if (roller_primary_pair_coherent) {
        /* A side whose upstream type is NONE did not validate its secondary.
         * Validate both rollback images before forcing either additional swap;
         * corruption or an ineligible security counter fails closed. */
        IMAGES_ITER(BOOT_CURR_IMG(state)) {
            boot_status_reset(&bs);
            FIH_CALL(boot_validate_slot, fih_rc,
                     state, BOOT_SECONDARY_SLOT, &bs);
            if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
                BOOT_LOG_ERR("Paired rollback image validation failed");
                rc = BOOT_EBADIMAGE;
                goto out;
            }
        }
        for (image_index = 0; image_index < BOOT_IMAGE_NUMBER; ++image_index) {
            state->swap_type[image_index] = BOOT_SWAP_TYPE_REVERT;
        }
    }

#endif

#if defined(MCUBOOT_HW_ROLLBACK_PROT) && !defined(MCUBOOT_OVERWRITE_ONLY)
    /* A paired release is committed only after both primary trailers are
     * explicitly confirmed and both headers remain valid. Until that point no
     * NV security counter may make the previous paired release ineligible. */
    {
        uint8_t roller_image_ok[BOOT_IMAGE_NUMBER];
        bool roller_commit_primary_verified[BOOT_IMAGE_NUMBER] = { false, false };
        RollerPair_ReleaseIdentity roller_commit_identity[BOOT_IMAGE_NUMBER];
        bool roller_commit_ready;

        IMAGES_ITER(BOOT_CURR_IMG(state)) {
            rc = boot_read_swap_state_by_id(
                    FLASH_AREA_IMAGE_PRIMARY(BOOT_CURR_IMG(state)),
                    &swap_state);
            if (rc != 0) {
                BOOT_LOG_ERR("Paired image_ok read failed");
                goto out;
            }
            if (swap_state.image_ok == BOOT_FLAG_BAD) {
                BOOT_LOG_ERR("Corrupt image_ok in paired release");
                rc = BOOT_EBADIMAGE;
                goto out;
            }
            roller_image_ok[BOOT_CURR_IMG(state)] =
                boot_is_header_valid(
                    boot_img_hdr(state, BOOT_PRIMARY_SLOT),
                    BOOT_IMG_AREA(state, BOOT_PRIMARY_SLOT)) ?
                    swap_state.image_ok : BOOT_FLAG_UNSET;
        }

        if (RollerPair_AllImagesConfirmed(roller_image_ok,
                                          BOOT_IMAGE_NUMBER,
                                          BOOT_FLAG_SET)) {
            /* Counters are irreversible.  Validate both signed primary images
             * in this boot before trusting their protected security-counter
             * TLVs, then require the complete paired identity to match. */
#if defined(MCUBOOT_DOUBLE_SIGN_VERIF)
            ImageValidIndex = 0U;
            for (image_index = 0; image_index < BOOT_IMAGE_NUMBER; ++image_index) {
                ImageValidStatus[image_index] = IMAGE_INVALID;
            }
            ImageValidEnable = 1U;
#endif
            IMAGES_ITER(BOOT_CURR_IMG(state)) {
                FIH_CALL(boot_validate_slot, fih_rc,
                         state, BOOT_PRIMARY_SLOT, NULL);
                if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
#if defined(MCUBOOT_DOUBLE_SIGN_VERIF)
                    ImageValidEnable = 0U;
#endif
                    BOOT_LOG_ERR("Paired confirmed primary validation failed");
                    rc = BOOT_EBADIMAGE;
                    goto out;
                }
                roller_commit_primary_verified[BOOT_CURR_IMG(state)] = true;
                rc = bootutil_get_img_security_cnt(
                        boot_img_hdr(state, BOOT_PRIMARY_SLOT),
                        BOOT_IMG_AREA(state, BOOT_PRIMARY_SLOT),
                        &roller_commit_identity[BOOT_CURR_IMG(state)]
                             .security_counter);
                if (rc != 0) {
                    BOOT_LOG_ERR("Paired confirmed release identity is invalid");
                    goto out;
                }
                roller_commit_identity[BOOT_CURR_IMG(state)].major =
                    boot_img_hdr(state, BOOT_PRIMARY_SLOT)->ih_ver.iv_major;
                roller_commit_identity[BOOT_CURR_IMG(state)].minor =
                    boot_img_hdr(state, BOOT_PRIMARY_SLOT)->ih_ver.iv_minor;
                roller_commit_identity[BOOT_CURR_IMG(state)].revision =
                    boot_img_hdr(state, BOOT_PRIMARY_SLOT)->ih_ver.iv_revision;
                roller_commit_identity[BOOT_CURR_IMG(state)].build =
                    boot_img_hdr(state, BOOT_PRIMARY_SLOT)->ih_ver.iv_build_num;
            }
#if defined(MCUBOOT_DOUBLE_SIGN_VERIF)
            ImageValidEnable = 0U;
            if (ImageValidIndex != BOOT_IMAGE_NUMBER) {
                BOOT_LOG_ERR("Paired confirmed double validation count failed");
                rc = BOOT_EBADIMAGE;
                goto out;
            }
            for (image_index = 0; image_index < BOOT_IMAGE_NUMBER; ++image_index) {
                if (ImageValidStatus[image_index] != IMAGE_VALID) {
                    BOOT_LOG_ERR("Paired confirmed double validation failed");
                    rc = BOOT_EBADIMAGE;
                    goto out;
                }
                /* The normal post-swap validation owns the final jump check. */
                ImageValidStatus[image_index] = IMAGE_INVALID;
            }
            ImageValidIndex = 0U;
#endif
            roller_commit_ready = RollerPair_ReleaseReadyToCommit(
                    roller_image_ok,
                    roller_commit_primary_verified,
                    roller_commit_identity,
                    BOOT_IMAGE_NUMBER,
                    BOOT_FLAG_SET);
            fih_rc = fih_int_encode_zero_equality(!roller_commit_ready);
            if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
                BOOT_LOG_ERR("Paired confirmed release identities do not match");
                rc = BOOT_EBADIMAGE;
                goto out;
            }
            /* Cache only a state that existed before this boot's swap loop.
             * A later FAIL path may clean bad staging, but may never create
             * confirmation for a test image on the application's behalf. */
            roller_primary_pair_preconfirmed = FIH_SUCCESS;
            IMAGES_ITER(BOOT_CURR_IMG(state)) {
                rc = boot_update_security_counter(
                        BOOT_CURR_IMG(state),
                        BOOT_PRIMARY_SLOT,
                        boot_img_hdr(state, BOOT_PRIMARY_SLOT),
                        &security_counter_updated);
                if (rc != 0) {
                    BOOT_LOG_ERR("Paired security counter update failed after "
                                 "image validation.");
                    goto out;
                }
                if (security_counter_updated) {
                    BOOT_LOG_INF("Paired security counter updated. Reset device");
                    NVIC_SystemReset();
                }
            }
        }
    }
#endif /* MCUBOOT_HW_ROLLBACK_PROT && !MCUBOOT_OVERWRITE_ONLY */
]=])
string(FIND "${OEMIROT_LOADER_SOURCE}" "${OEMIROT_LOADER_CONFIRM_OLD}"
       OEMIROT_LOADER_CONFIRM_POSITION)
if(OEMIROT_LOADER_CONFIRM_POSITION EQUAL -1)
  message(FATAL_ERROR
    "Pinned MCUboot loader changed: independent image confirmation block was not found")
endif()
string(REPLACE "${OEMIROT_LOADER_CONFIRM_OLD}" "${OEMIROT_LOADER_CONFIRM_NEW}"
       OEMIROT_LOADER_SOURCE "${OEMIROT_LOADER_SOURCE}")

# Run paired-target validation only after upstream dependency resolution. A
# power loss while Ethernet OTA is still staging one padded image can expose a
# legitimate one-sided TEST trailer; dependencies must first cancel that
# incomplete request so the already-confirmed old pair remains bootable.
set(OEMIROT_LOADER_TARGET_GATE_ANCHOR_OLD [=[
    /* Iterate over all the images. At this point there are no aborted swaps
     * and the swap types are determined for each image. By the end of the loop
     * all required update operations will have been finished.
     */
]=])
set(OEMIROT_LOADER_TARGET_GATE_ANCHOR_NEW [=[
#if (BOOT_IMAGE_NUMBER == 2) && !defined(MCUBOOT_PRIMARY_ONLY)
    /* Treat the post-operation images as one signed target. This catches a
     * manually assembled or faulted pair whose two independently valid images
     * reuse a version but carry different security counters (or vice versa).
     * It also covers a forward/revert operation resumed after one side has
     * already reached its primary slot. */
    roller_target_pair_required =
        RollerPair_SwapTypesFormOperation(state->swap_type,
                                          BOOT_IMAGE_NUMBER,
                                          BOOT_SWAP_TYPE_NONE,
                                          BOOT_SWAP_TYPE_TEST) ||
        RollerPair_SwapTypesFormOperation(state->swap_type,
                                          BOOT_IMAGE_NUMBER,
                                          BOOT_SWAP_TYPE_NONE,
                                          BOOT_SWAP_TYPE_REVERT);
    if (roller_target_pair_required) {
        for (image_index = 0; image_index < BOOT_IMAGE_NUMBER; ++image_index) {
            BOOT_CURR_IMG(state) = image_index;
            roller_target_slot[image_index] =
                (state->swap_type[image_index] == BOOT_SWAP_TYPE_NONE) ?
                BOOT_PRIMARY_SLOT : BOOT_SECONDARY_SLOT;
            boot_status_reset(&bs);
            FIH_CALL(boot_validate_slot, fih_rc,
                     state, roller_target_slot[image_index], &bs);
            if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
                BOOT_LOG_ERR("Paired target image validation failed");
                rc = BOOT_EBADIMAGE;
                goto out;
            }
            rc = bootutil_get_img_security_cnt(
                    &state->imgs[image_index]
                                [roller_target_slot[image_index]].hdr,
                    state->imgs[image_index]
                               [roller_target_slot[image_index]].area,
                    &roller_target_identity[image_index].security_counter);
            if (rc != 0) {
                BOOT_LOG_ERR("Paired target release identity is invalid");
                goto out;
            }
            roller_target_identity[image_index].major =
                state->imgs[image_index][roller_target_slot[image_index]]
                    .hdr.ih_ver.iv_major;
            roller_target_identity[image_index].minor =
                state->imgs[image_index][roller_target_slot[image_index]]
                    .hdr.ih_ver.iv_minor;
            roller_target_identity[image_index].revision =
                state->imgs[image_index][roller_target_slot[image_index]]
                    .hdr.ih_ver.iv_revision;
            roller_target_identity[image_index].build =
                state->imgs[image_index][roller_target_slot[image_index]]
                    .hdr.ih_ver.iv_build_num;
        }
        if (!RollerPair_ReleaseIdentitiesMatch(&roller_target_identity[0],
                                               &roller_target_identity[1])) {
            BOOT_LOG_ERR("Paired target release identities do not match");
            rc = BOOT_EBADIMAGE;
            goto out;
        }
    }
#endif

    /* Iterate over all the images. At this point there are no aborted swaps
     * and the swap types are determined for each image. By the end of the loop
     * all required update operations will have been finished.
     */
]=])
string(FIND "${OEMIROT_LOADER_SOURCE}"
       "${OEMIROT_LOADER_TARGET_GATE_ANCHOR_OLD}"
       OEMIROT_LOADER_TARGET_GATE_ANCHOR_POSITION)
if(OEMIROT_LOADER_TARGET_GATE_ANCHOR_POSITION EQUAL -1)
  message(FATAL_ERROR
    "Pinned MCUboot loader changed: post-dependency target gate anchor was not found")
endif()
string(REPLACE "${OEMIROT_LOADER_TARGET_GATE_ANCHOR_OLD}"
       "${OEMIROT_LOADER_TARGET_GATE_ANCHOR_NEW}"
       OEMIROT_LOADER_SOURCE "${OEMIROT_LOADER_SOURCE}")

# Upstream FAIL recovery writes image_ok to suppress another revert.  That is
# safe only for a primary pair which was already exactly confirmed, matching,
# and fully verified before this boot's swap loop.  Otherwise corrupt rollback
# slots could make the bootloader confirm an untested application release.
set(OEMIROT_LOADER_FAIL_CONFIRM_OLD [=[
        case BOOT_SWAP_TYPE_FAIL:
            /* The image in secondary slot was invalid and is now erased. Ensure
             * we don't try to boot into it again on the next reboot. Do this by
             * pretending we just reverted back to primary slot.
             */
#ifndef MCUBOOT_OVERWRITE_ONLY
            /* image_ok needs to be explicitly set to avoid a new revert. */
            rc = swap_set_image_ok(BOOT_CURR_IMG(state));
]=])
set(OEMIROT_LOADER_FAIL_CONFIRM_NEW [=[
        case BOOT_SWAP_TYPE_FAIL:
            /* The image in secondary slot was invalid and is now erased. Ensure
             * we don't try to boot into it again on the next reboot. Do this by
             * pretending we just reverted back to primary slot.
             */
#ifndef MCUBOOT_OVERWRITE_ONLY
#if (BOOT_IMAGE_NUMBER == 2) && !defined(MCUBOOT_PRIMARY_ONLY)
            if (fih_not_eq(roller_primary_pair_preconfirmed, FIH_SUCCESS)) {
                BOOT_LOG_ERR("Unconfirmed primary cannot consume failed staging");
                BOOT_SWAP_TYPE(state) = BOOT_SWAP_TYPE_PANIC;
                rc = BOOT_EBADIMAGE;
                FIH_PANIC;
                goto out;
            }
#endif
            /* The trusted pair was already confirmed before this loop, so this
             * call is idempotent and can never synthesize application health. */
            rc = swap_set_image_ok(BOOT_CURR_IMG(state));
]=])
string(FIND "${OEMIROT_LOADER_SOURCE}"
       "${OEMIROT_LOADER_FAIL_CONFIRM_OLD}"
       OEMIROT_LOADER_FAIL_CONFIRM_POSITION)
if(OEMIROT_LOADER_FAIL_CONFIRM_POSITION EQUAL -1)
  message(FATAL_ERROR
    "Pinned MCUboot loader changed: failed-staging confirmation path was not found")
endif()
string(REPLACE "${OEMIROT_LOADER_FAIL_CONFIRM_OLD}"
       "${OEMIROT_LOADER_FAIL_CONFIRM_NEW}"
       OEMIROT_LOADER_SOURCE "${OEMIROT_LOADER_SOURCE}")

# Remember which final primaries passed MCUboot's complete cryptographic
# validation.  The countermeasure runs later and must not infer this from a
# structurally valid header alone.
set(OEMIROT_LOADER_PRIMARY_VERIFIED_OLD [=[
        FIH_CALL(boot_validate_slot, fih_rc, state, BOOT_PRIMARY_SLOT, NULL);
        if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
            goto out;
        }
#if defined(MCUBOOT_DOUBLE_SIGN_VERIF) || defined(MCUBOOT_USE_HASH_REF)
]=])
set(OEMIROT_LOADER_PRIMARY_VERIFIED_NEW [=[
        FIH_CALL(boot_validate_slot, fih_rc, state, BOOT_PRIMARY_SLOT, NULL);
        if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
            goto out;
        }
#if (BOOT_IMAGE_NUMBER == 2) && !defined(MCUBOOT_PRIMARY_ONLY)
        roller_final_primary_verified[BOOT_CURR_IMG(state)] = true;
#endif
#if defined(MCUBOOT_DOUBLE_SIGN_VERIF) || defined(MCUBOOT_USE_HASH_REF)
]=])
string(FIND "${OEMIROT_LOADER_SOURCE}"
       "${OEMIROT_LOADER_PRIMARY_VERIFIED_OLD}"
       OEMIROT_LOADER_PRIMARY_VERIFIED_POSITION)
if(OEMIROT_LOADER_PRIMARY_VERIFIED_POSITION EQUAL -1)
  message(FATAL_ERROR
    "Pinned MCUboot loader changed: final primary validation anchor was not found")
endif()
string(REPLACE "${OEMIROT_LOADER_PRIMARY_VERIFIED_OLD}"
       "${OEMIROT_LOADER_PRIMARY_VERIFIED_NEW}"
       OEMIROT_LOADER_SOURCE "${OEMIROT_LOADER_SOURCE}")

set(OEMIROT_LOADER_COUNTERMEASURE_ANCHOR_OLD [=[
    /* Iterate over all the images. */
    IMAGES_ITER(BOOT_CURR_IMG(state)) {

#if !defined(MCUBOOT_OVERWRITE_ONLY)
        /* Security counters will be updated if the active image is confirmed:
]=])
set(OEMIROT_LOADER_COUNTERMEASURE_ANCHOR_NEW [=[
#if !defined(MCUBOOT_OVERWRITE_ONLY)
    uint8_t roller_countermeasure_image_ok[BOOT_IMAGE_NUMBER];
    RollerPair_ReleaseIdentity roller_countermeasure_identity[BOOT_IMAGE_NUMBER];
    bool roller_countermeasure_all_confirmed;
    bool roller_countermeasure_ready;

    IMAGES_ITER(BOOT_CURR_IMG(state)) {
        rc = boot_read_swap_state_by_id(
                FLASH_AREA_IMAGE_PRIMARY(BOOT_CURR_IMG(state)),
                &swap_state);
        if (rc != 0) {
            BOOT_LOG_ERR("Paired countermeasure image_ok read failed");
            goto out;
        }
        if (swap_state.image_ok == BOOT_FLAG_BAD) {
            BOOT_LOG_ERR("Corrupt image_ok during paired countermeasure");
            rc = BOOT_EBADIMAGE;
            goto out;
        }
        roller_countermeasure_image_ok[BOOT_CURR_IMG(state)] =
            boot_is_header_valid(
                boot_img_hdr(state, BOOT_PRIMARY_SLOT),
                BOOT_IMG_AREA(state, BOOT_PRIMARY_SLOT)) ?
                swap_state.image_ok : BOOT_FLAG_UNSET;
        rc = bootutil_get_img_security_cnt(
                boot_img_hdr(state, BOOT_PRIMARY_SLOT),
                BOOT_IMG_AREA(state, BOOT_PRIMARY_SLOT),
                &roller_countermeasure_identity[BOOT_CURR_IMG(state)]
                     .security_counter);
        if (rc != 0) {
            BOOT_LOG_ERR("Paired countermeasure release identity is invalid");
            goto out;
        }
        roller_countermeasure_identity[BOOT_CURR_IMG(state)].major =
            boot_img_hdr(state, BOOT_PRIMARY_SLOT)->ih_ver.iv_major;
        roller_countermeasure_identity[BOOT_CURR_IMG(state)].minor =
            boot_img_hdr(state, BOOT_PRIMARY_SLOT)->ih_ver.iv_minor;
        roller_countermeasure_identity[BOOT_CURR_IMG(state)].revision =
            boot_img_hdr(state, BOOT_PRIMARY_SLOT)->ih_ver.iv_revision;
        roller_countermeasure_identity[BOOT_CURR_IMG(state)].build =
            boot_img_hdr(state, BOOT_PRIMARY_SLOT)->ih_ver.iv_build_num;
    }
    roller_countermeasure_all_confirmed =
        RollerPair_AllImagesConfirmed(roller_countermeasure_image_ok,
                                      BOOT_IMAGE_NUMBER,
                                      BOOT_FLAG_SET);
    roller_countermeasure_ready = RollerPair_ReleaseReadyToCommit(
        roller_countermeasure_image_ok,
        roller_final_primary_verified,
        roller_countermeasure_identity,
        BOOT_IMAGE_NUMBER,
        BOOT_FLAG_SET);
    if (roller_countermeasure_all_confirmed) {
        fih_rc = fih_int_encode_zero_equality(!roller_countermeasure_ready);
        if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
            BOOT_LOG_ERR("Paired countermeasure release is not coherent");
            rc = BOOT_EBADIMAGE;
            goto out;
        }
    }
#endif

    /* Iterate over all the images. */
    IMAGES_ITER(BOOT_CURR_IMG(state)) {

#if !defined(MCUBOOT_OVERWRITE_ONLY)
        /* Security counters will be updated if the active image is confirmed:
]=])
string(FIND "${OEMIROT_LOADER_SOURCE}"
       "${OEMIROT_LOADER_COUNTERMEASURE_ANCHOR_OLD}"
       OEMIROT_LOADER_COUNTERMEASURE_ANCHOR_POSITION)
if(OEMIROT_LOADER_COUNTERMEASURE_ANCHOR_POSITION EQUAL -1)
  message(FATAL_ERROR
    "Pinned MCUboot loader changed: countermeasure loop was not found")
endif()
string(REPLACE "${OEMIROT_LOADER_COUNTERMEASURE_ANCHOR_OLD}"
       "${OEMIROT_LOADER_COUNTERMEASURE_ANCHOR_NEW}"
       OEMIROT_LOADER_SOURCE "${OEMIROT_LOADER_SOURCE}")

set(OEMIROT_LOADER_COUNTER_CONDITION_OLD [=[
        if ((swap_state.image_ok != BOOT_FLAG_UNSET) &&
            (boot_is_header_valid(boot_img_hdr(state, BOOT_PRIMARY_SLOT),
                                  BOOT_IMG_AREA(state, BOOT_PRIMARY_SLOT)))) {
]=])
set(OEMIROT_LOADER_COUNTER_CONDITION_NEW [=[
        if (roller_countermeasure_ready &&
            (swap_state.image_ok == BOOT_FLAG_SET) &&
            (boot_is_header_valid(boot_img_hdr(state, BOOT_PRIMARY_SLOT),
                                  BOOT_IMG_AREA(state, BOOT_PRIMARY_SLOT)))) {
]=])
string(REGEX MATCHALL
       "if \\(\\(swap_state[.]image_ok != BOOT_FLAG_UNSET\\)"
       OEMIROT_LOADER_COUNTER_CONDITION_MATCHES "${OEMIROT_LOADER_SOURCE}")
list(LENGTH OEMIROT_LOADER_COUNTER_CONDITION_MATCHES
     OEMIROT_LOADER_COUNTER_CONDITION_COUNT)
if(NOT OEMIROT_LOADER_COUNTER_CONDITION_COUNT EQUAL 1)
  message(FATAL_ERROR
    "Pinned MCUboot loader changed: expected one independent countermeasure condition, found ${OEMIROT_LOADER_COUNTER_CONDITION_COUNT}")
endif()
string(REPLACE "${OEMIROT_LOADER_COUNTER_CONDITION_OLD}"
       "${OEMIROT_LOADER_COUNTER_CONDITION_NEW}"
       OEMIROT_LOADER_SOURCE "${OEMIROT_LOADER_SOURCE}")

# Release builds define NDEBUG, so an I/O error in the countermeasure's second
# trailer sample must never be hidden behind upstream assert().
set(OEMIROT_LOADER_COUNTER_READ_OLD [=[
        rc = boot_read_swap_state_by_id(FLASH_AREA_IMAGE_PRIMARY(BOOT_CURR_IMG(state)),
                                        &swap_state);
        assert(rc == 0);

        /* Check if primary slot contains a confirmed image */
]=])
set(OEMIROT_LOADER_COUNTER_READ_NEW [=[
        rc = boot_read_swap_state_by_id(FLASH_AREA_IMAGE_PRIMARY(BOOT_CURR_IMG(state)),
                                        &swap_state);
        if (rc != 0) {
            BOOT_LOG_ERR("Paired countermeasure update image_ok read failed");
            goto out;
        }

        /* Check if primary slot contains a confirmed image */
]=])
string(FIND "${OEMIROT_LOADER_SOURCE}"
       "${OEMIROT_LOADER_COUNTER_READ_OLD}"
       OEMIROT_LOADER_COUNTER_READ_POSITION)
if(OEMIROT_LOADER_COUNTER_READ_POSITION EQUAL -1)
  message(FATAL_ERROR
    "Pinned MCUboot loader changed: countermeasure trailer re-read was not found")
endif()
string(REPLACE "${OEMIROT_LOADER_COUNTER_READ_OLD}"
       "${OEMIROT_LOADER_COUNTER_READ_NEW}"
       OEMIROT_LOADER_SOURCE "${OEMIROT_LOADER_SOURCE}")

# Even if an interrupted swap is downgraded to NONE/FAIL because its remaining
# secondary slot became unreadable, never hand an old/new primary pair to the
# applications. This final gate runs after MCUboot's cryptographic validation
# of both primaries and checks the complete signed release identity once more.
set(OEMIROT_LOADER_FINAL_PAIR_ANCHOR_OLD [=[
#if (BOOT_IMAGE_NUMBER > 1)
    /* Always boot from the primary slot of Image 0. */
    BOOT_CURR_IMG(state) = 0;
#endif
]=])
set(OEMIROT_LOADER_FINAL_PAIR_ANCHOR_NEW [=[
#if (BOOT_IMAGE_NUMBER == 2) && !defined(MCUBOOT_PRIMARY_ONLY)
    {
        RollerPair_ReleaseIdentity roller_final_identity[BOOT_IMAGE_NUMBER];
        int roller_final_validation_incomplete;
        int roller_final_identity_mismatch;

#if defined(MCUBOOT_DOUBLE_SIGN_VERIF)
        roller_final_validation_incomplete =
            (ImageValidIndex != BOOT_IMAGE_NUMBER);
        for (image_index = 0; image_index < BOOT_IMAGE_NUMBER; ++image_index) {
            roller_final_validation_incomplete |=
                (ImageValidStatus[image_index] != IMAGE_VALID);
        }
        fih_rc = fih_int_encode_zero_equality(
            roller_final_validation_incomplete);
        if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
            BOOT_LOG_ERR("Final paired primary double validation is incomplete");
            rc = BOOT_EBADIMAGE;
            goto out;
        }
#endif
        roller_final_validation_incomplete = 0;
        for (image_index = 0; image_index < BOOT_IMAGE_NUMBER; ++image_index) {
            roller_final_validation_incomplete |=
                !roller_final_primary_verified[image_index];
        }
        fih_rc = fih_int_encode_zero_equality(
            roller_final_validation_incomplete);
        if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
            BOOT_LOG_ERR("Final paired primary validation is incomplete");
            rc = BOOT_EBADIMAGE;
            goto out;
        }

        for (image_index = 0; image_index < BOOT_IMAGE_NUMBER; ++image_index) {
            rc = bootutil_get_img_security_cnt(
                    &state->imgs[image_index][BOOT_PRIMARY_SLOT].hdr,
                    state->imgs[image_index][BOOT_PRIMARY_SLOT].area,
                    &roller_final_identity[image_index].security_counter);
            if (rc != 0) {
                BOOT_LOG_ERR("Final paired primary identity is invalid");
                goto out;
            }
            roller_final_identity[image_index].major =
                state->imgs[image_index][BOOT_PRIMARY_SLOT].hdr.ih_ver.iv_major;
            roller_final_identity[image_index].minor =
                state->imgs[image_index][BOOT_PRIMARY_SLOT].hdr.ih_ver.iv_minor;
            roller_final_identity[image_index].revision =
                state->imgs[image_index][BOOT_PRIMARY_SLOT].hdr.ih_ver.iv_revision;
            roller_final_identity[image_index].build =
                state->imgs[image_index][BOOT_PRIMARY_SLOT].hdr.ih_ver.iv_build_num;
        }
        roller_final_identity_mismatch =
            !RollerPair_ReleaseIdentitiesMatch(&roller_final_identity[0],
                                               &roller_final_identity[1]);
        fih_rc = fih_int_encode_zero_equality(
            roller_final_identity_mismatch);
        if (fih_not_eq(fih_rc, FIH_SUCCESS)) {
            BOOT_LOG_ERR("Final paired primary release identities do not match");
            rc = BOOT_EBADIMAGE;
            goto out;
        }
    }
#endif

#if (BOOT_IMAGE_NUMBER > 1)
    /* Always boot from the primary slot of Image 0. */
    BOOT_CURR_IMG(state) = 0;
#endif
]=])
string(FIND "${OEMIROT_LOADER_SOURCE}"
       "${OEMIROT_LOADER_FINAL_PAIR_ANCHOR_OLD}"
       OEMIROT_LOADER_FINAL_PAIR_ANCHOR_POSITION)
if(OEMIROT_LOADER_FINAL_PAIR_ANCHOR_POSITION EQUAL -1)
  message(FATAL_ERROR
    "Pinned MCUboot loader changed: final primary-pair gate anchor was not found")
endif()
string(REPLACE "${OEMIROT_LOADER_FINAL_PAIR_ANCHOR_OLD}"
       "${OEMIROT_LOADER_FINAL_PAIR_ANCHOR_NEW}"
       OEMIROT_LOADER_SOURCE "${OEMIROT_LOADER_SOURCE}")
set(OEMIROT_LOADER_DERIVATIVE
    "${CMAKE_CURRENT_BINARY_DIR}/loader_roller_ecu.c")
file(WRITE "${OEMIROT_LOADER_DERIVATIVE}" "${OEMIROT_LOADER_SOURCE}")

foreach(required_file
    "${OEMIROT_EXAMPLE_ROOT}/Src/boot_hal.c"
    "${OEMIROT_EXAMPLE_ROOT}/Src/startup_stm32h5xx.c"
    "${OEMIROT_EXAMPLE_ROOT}/STM32CubeIDE/stm32h5xx_bl2.ld"
    "${MCUBOOT_ROOT}/boot/bootutil/src/loader.c"
    "${MBEDCRYPTO_ROOT}/library/ecdsa.c")
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR
      "STM32CubeH5 v1.7.0 OEMiROT dependency missing: ${required_file}")
  endif()
endforeach()

execute_process(
  COMMAND ${CMAKE_C_COMPILER} -E -P -xc
          -DSTM32H563xx -DBL2
          -I${CMAKE_CURRENT_SOURCE_DIR}/Config
          -I${CMAKE_CURRENT_SOURCE_DIR}/../../Shared
          -I${OEMIROT_EXAMPLE_ROOT}/Inc
          -I${STM32CUBE_H5_ROOT}/Drivers/CMSIS/Include
          -I${STM32CUBE_H5_ROOT}/Drivers/CMSIS/Device/ST/STM32H5xx/Include
          -I${STM32CUBE_H5_ROOT}/Drivers/STM32H5xx_HAL_Driver/Inc
          ${OEMIROT_EXAMPLE_ROOT}/STM32CubeIDE/stm32h5xx_bl2.ld
  OUTPUT_FILE ${CMAKE_CURRENT_BINARY_DIR}/oemirot.ld
  RESULT_VARIABLE linker_preprocess_result
  ERROR_VARIABLE linker_preprocess_error)
if(NOT linker_preprocess_result EQUAL 0)
  message(FATAL_ERROR "OEMiROT linker preprocessing failed: ${linker_preprocess_error}")
endif()

set(OEMIROT_LOCAL_SOURCES
  bl2_nv_services.c
  keys_map.c
  low_level_com.c
  low_level_flash.c
  low_level_obkeys.c
  low_level_obkeys_device.c
  low_level_rng.c
  mpu_armv8m_drv.c
  sha256_alt.c
  stm32h5xx_hal_msp.c
  system_stm32h5xx.c
  tick.c)
list(TRANSFORM OEMIROT_LOCAL_SOURCES
     PREPEND "${OEMIROT_EXAMPLE_ROOT}/Src/")

set(HAL_SOURCES
  stm32h5xx_hal.c
  stm32h5xx_hal_cortex.c
  stm32h5xx_hal_dma.c
  stm32h5xx_hal_dma_ex.c
  stm32h5xx_hal_exti.c
  stm32h5xx_hal_flash.c
  stm32h5xx_hal_flash_ex.c
  stm32h5xx_hal_gpio.c
  stm32h5xx_hal_gtzc.c
  stm32h5xx_hal_hash.c
  stm32h5xx_hal_icache.c
  stm32h5xx_hal_pka.c
  stm32h5xx_hal_pwr.c
  stm32h5xx_hal_pwr_ex.c
  stm32h5xx_hal_rcc.c
  stm32h5xx_hal_rcc_ex.c
  stm32h5xx_hal_rng.c
  stm32h5xx_hal_rng_ex.c
  stm32h5xx_hal_rtc.c
  stm32h5xx_hal_rtc_ex.c
  stm32h5xx_hal_uart.c
  stm32h5xx_hal_uart_ex.c)
list(TRANSFORM HAL_SOURCES
     PREPEND "${STM32CUBE_H5_ROOT}/Drivers/STM32H5xx_HAL_Driver/Src/")

set(MBEDCRYPTO_SOURCES
  aes.c
  asn1parse.c
  bignum.c
  bignum_core.c
  cipher.c
  constant_time.c
  hmac_drbg.c
  ecdh.c
  ecdsa.c
  ecp.c
  ecp_curves.c
  entropy.c
  hkdf.c
  md.c
  memory_buffer_alloc.c
  platform.c
  platform_util.c
  sha256.c)
list(TRANSFORM MBEDCRYPTO_SOURCES PREPEND "${MBEDCRYPTO_ROOT}/library/")

set(MCUBOOT_SOURCES
  "${MCUBOOT_ROOT}/bl2/ext/mcuboot/bl2_main.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/bootutil_misc.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/caps.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/encrypted.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/fault_injection_hardening.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/fault_injection_hardening_delay_rng_mbedtls.c"
  "${MCUBOOT_ROOT}/bl2/src/flash_map.c"
  "${MCUBOOT_ROOT}/bl2/ext/mcuboot/flash_map_extended.c"
  "${MCUBOOT_ROOT}/bl2/ext/mcuboot/flash_map_legacy.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/image_ec.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/image_ec256.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/image_validate.c"
  "${OEMIROT_LOADER_DERIVATIVE}"
  "${MCUBOOT_ROOT}/bl2/src/security_cnt.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/swap_misc.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/swap_move.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/swap_scratch.c"
  "${MCUBOOT_ROOT}/boot/bootutil/src/tlv.c"
  "${MCUBOOT_ROOT}/platform/ext/common/uart_stdout.c")

add_executable(${PROJECT_NAME}
  ${OEMIROT_LOCAL_SOURCES}
  ${OEMIROT_BOOT_HAL_DERIVATIVE}
  ${OEMIROT_STARTUP_DERIVATIVE}
  ${OEMIROT_SECURITY_DERIVATIVE}
  ${HAL_SOURCES}
  ${MBEDCRYPTO_SOURCES}
  ${MCUBOOT_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/low_level_device.c)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Config
  ${CMAKE_CURRENT_SOURCE_DIR}/../../Shared
  ${OEMIROT_EXAMPLE_ROOT}/Inc
  ${STM32CUBE_H5_ROOT}/Drivers/STM32H5xx_HAL_Driver/Inc
  ${STM32CUBE_H5_ROOT}/Drivers/CMSIS/Include
  ${STM32CUBE_H5_ROOT}/Drivers/CMSIS/Device/ST/STM32H5xx/Include
  ${MBEDCRYPTO_ROOT}/include
  ${MBEDCRYPTO_ROOT}/include/mbedtls
  ${MCUBOOT_ROOT}/platform/include
  ${MCUBOOT_ROOT}
  ${MCUBOOT_ROOT}/platform/ext/common
  ${MCUBOOT_ROOT}/platform/ext/driver
  ${MCUBOOT_ROOT}/bl2/include
  ${MCUBOOT_ROOT}/bl2/ext/mcuboot
  ${MCUBOOT_ROOT}/bl2/ext/mcuboot/include
  ${MCUBOOT_ROOT}/boot/bootutil/include
  ${MCUBOOT_ROOT}/boot/bootutil/src
  ${MCUBOOT_ROOT}/ext/tinycrypt/lib/include)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  STM32H563xx
  USE_HAL_DRIVER
  MBEDTLS_CONFIG_FILE=<mbedtls_config.h>
  BOOT_MAX_ALIGN=16
  BL2
  MCUBOOT_DOUBLE_SIGN_VERIF
  FLOW_CONTROL
  NDEBUG
  DEFAULT_UART_BAUDRATE=115200)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions(${PROJECT_NAME} PRIVATE OEMIROT_DEV_MODE)
endif()
if(ROLLER_OEMIROT_OPEN_BENCH)
  target_compile_definitions(${PROJECT_NAME} PRIVATE ROLLER_OEMIROT_OPEN_BENCH)
endif()

target_compile_options(${PROJECT_NAME} PRIVATE
  -Os
  -fno-common
  -ffunction-sections
  -fdata-sections
  -Wno-unused-parameter
  # Several pinned ST headers include region_defs.h/flash_layout.h with quotes
  # from inside the reference Inc directory.  GCC searches that directory
  # before -I paths, which would silently select the NUCLEO example layout.
  # Force the product layout into every translation unit first; the upstream
  # include guards then prevent any reference-board layout from replacing it.
  -include${CMAKE_CURRENT_SOURCE_DIR}/Config/flash_layout.h
  -include${CMAKE_CURRENT_SOURCE_DIR}/Config/region_defs.h)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary
          $<TARGET_FILE:${PROJECT_NAME}>
          ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.bin
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
  VERBATIM)
