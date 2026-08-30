cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Compiler options
set(STM32_MCU_FLAGS  "-mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mcmse " )

#Linker options
option(ECU_OEMIROT_LAYOUT "Link the Secure app into its MCUboot primary slot" OFF)
option(ECU_FACTORY_PROVISIONING "Build irreversible ATECC factory service" OFF)
 # CubeMX owns the input linker script. Always generate a derivative in the
 # build tree so both development and signed builds reserve the Secure
 # persistent store. Fail closed if CubeMX changes the reviewed declarations.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/STM32H563xx_FLASH_s.ld" ECU_LINKER_TEXT)
set(ECU_LINKER_STACK_OLD
    "_estack = ORIGIN(RAM) + LENGTH(RAM); /* end of \"RAM\" Ram type memory */")
set(ECU_LINKER_STACK_NEW
    "_estack = 0x3003FB80; /* Secure stack stops below OEMiROT UID handoff. */")
set(ECU_LINKER_FLASH_OLD
    "FLASH    (rx)    : ORIGIN = 0xc000000,    LENGTH = 1000K    /* Sectors 125/126 are reserved for Secure valve parameters. */")
set(ECU_LINKER_NSC_OLD
    "FLASH_NSC    (rx)    : ORIGIN = 0xc0fe000,    LENGTH = 8K    /* Non-Secure Call-able region */")
string(FIND "${ECU_LINKER_TEXT}" "${ECU_LINKER_FLASH_OLD}" ECU_LINKER_FLASH_POS)
string(FIND "${ECU_LINKER_TEXT}" "${ECU_LINKER_NSC_OLD}" ECU_LINKER_NSC_POS)
string(FIND "${ECU_LINKER_TEXT}" "${ECU_LINKER_STACK_OLD}" ECU_LINKER_STACK_POS)
if(ECU_LINKER_FLASH_POS EQUAL -1 OR ECU_LINKER_NSC_POS EQUAL -1 OR
   ECU_LINKER_STACK_POS EQUAL -1)
    message(FATAL_ERROR "CubeMX Secure linker layout changed; review persistent/OEMiROT relocation")
endif()
string(REPLACE "${ECU_LINKER_STACK_OLD}" "${ECU_LINKER_STACK_NEW}"
       ECU_LINKER_TEXT "${ECU_LINKER_TEXT}")
if(ECU_OEMIROT_LAYOUT)
    set(ECU_LINKER_FLASH_NEW
        "FLASH    (rx)    : ORIGIN = 0x0c030400,   LENGTH = 0x2d800 /* OEMiROT S body below the reviewed NSC/TLV/trailer windows. */")
    set(ECU_LINKER_NSC_NEW
        "FLASH_NSC (rx)    : ORIGIN = 0x0c05dc00,   LENGTH = 0x400   /* NSC veneer remains inside the signed image body. */")
    string(REPLACE "${ECU_LINKER_NSC_OLD}" "${ECU_LINKER_NSC_NEW}"
           ECU_LINKER_TEXT "${ECU_LINKER_TEXT}")
    set(ECU_LINKER_NAME STM32H563xx_OEMiROT_s.ld)
else()
    set(ECU_LINKER_FLASH_NEW
        "FLASH    (rx)    : ORIGIN = 0x0c000000,   LENGTH = 0x0e0000 /* Development image; Secure persistent store excluded. */")
    set(ECU_LINKER_NAME STM32H563xx_DEVELOPMENT_s.ld)
endif()
string(REPLACE "${ECU_LINKER_FLASH_OLD}" "${ECU_LINKER_FLASH_NEW}"
       ECU_LINKER_TEXT "${ECU_LINKER_TEXT}")
set(ECU_LINKER_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${ECU_LINKER_NAME}")
file(WRITE "${ECU_LINKER_OUTPUT}" "${ECU_LINKER_TEXT}")
file(RELATIVE_PATH STM32_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}"
     "${ECU_LINKER_OUTPUT}")
set(ECU_NSC_ABI_BASELINE_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../Secure_nsclib/secure_nsc_abi_v3.s")
set(ECU_NSC_ABI_BASELINE_OBJECT
    "${CMAKE_CURRENT_BINARY_DIR}/secure_nsc_abi_v3.o")
if(ECU_OEMIROT_LAYOUT)
    # Keep the release import library private to this build tree. Development
    # and CubeMX/CubeIDE builds use a different NSC address window and must
    # never be able to overwrite an OEMiROT NonSecure link input.
    set(ECU_NSC_IMPORT_LIBRARY
        "${CMAKE_CURRENT_BINARY_DIR}/secure_nsclib.o")
    set(STM32_LINKER_OPTION
        "-Wl,--cmse-implib -Wl,--in-implib=${ECU_NSC_ABI_BASELINE_OBJECT} -Wl,--out-implib=${ECU_NSC_IMPORT_LIBRARY}")
else()
    set(ECU_NSC_IMPORT_LIBRARY
        "${CMAKE_CURRENT_SOURCE_DIR}/../Secure_nsclib/secure_nsclib.o")
    set(STM32_LINKER_OPTION
        "-Wl,--cmse-implib -Wl,--out-implib=${ECU_NSC_IMPORT_LIBRARY}")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME ECU_S)
project(${CMAKE_PROJECT_NAME})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Include mx-generated
include("mx-generated.cmake")

# Keep the Secure ownership and timing contract persistent across CubeMX
# regeneration. ES0565 section 2.2.34 requires identical security and
# privilege attribution for both FDCAN instances because their message RAM
# crosses a 1 KiB TrustZone boundary.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../ECU.ioc" ECU_IOC_TEXT)
foreach(ECU_FDCAN_IOC_REQUIRED IN ITEMS
    "FDCAN1.AutoRetransmission=ENABLE"
    "FDCAN1.CalculateBaudRateNominal=250000"
    "FDCAN1.NominalPrescaler=4"
    "FDCAN1.NominalSyncJumpWidth=4"
    "FDCAN1.NominalTimeSeg1=19"
    "FDCAN1.NominalTimeSeg2=5"
    "FDCAN1.TransmitPause=ENABLE"
    "FDCAN1.StdFiltersNbr=0"
    "FDCAN1.ExtFiltersNbr=6"
    "FDCAN2.AutoRetransmission=DISABLE"
    "FDCAN2.CalculateBaudRateNominal=250000"
    "FDCAN2.NominalPrescaler=4"
    "FDCAN2.NominalSyncJumpWidth=4"
    "FDCAN2.NominalTimeSeg1=19"
    "FDCAN2.NominalTimeSeg2=5"
    "FDCAN2.TransmitPause=ENABLE"
    "FDCAN2.StdFiltersNbr=0"
    "FDCAN2.ExtFiltersNbr=2"
    "GTZC_S.FDCAN1_Secure=GTZC_TZSC_PERIPH_SEC"
    "GTZC_S.FDCAN1_Illegal_Access_Interrupt=Enabled"
    "GTZC_S.FDCAN2_Secure=GTZC_TZSC_PERIPH_SEC"
    "GTZC_S.FDCAN2_Illegal_Access_Interrupt=Enabled"
    "PD0.ContextOwner=CortexM33S"
    "PD0.PinAttribute=CortexM33S"
    "PD0.Signal=FDCAN1_RX"
    "PD1.ContextOwner=CortexM33S"
    "PD1.PinAttribute=CortexM33S"
    "PD1.Signal=FDCAN1_TX"
    "PB12.ContextOwner=CortexM33S"
    "PB12.PinAttribute=CortexM33S"
    "PB12.Signal=FDCAN2_RX"
    "PB13.ContextOwner=CortexM33S"
    "PB13.PinAttribute=CortexM33S"
    "PB13.Signal=FDCAN2_TX"
    "PB6.PinAttribute=CortexM33S"
    "PB6.Signal=S_TIM4_CH1"
    "NVIC1.FDCAN1_IT0_IRQn=true\\:5"
    "NVIC1.FDCAN2_IT0_IRQn=true\\:2")
    string(FIND "${ECU_IOC_TEXT}" "${ECU_FDCAN_IOC_REQUIRED}"
           ECU_FDCAN_IOC_REQUIRED_POS)
    if(ECU_FDCAN_IOC_REQUIRED_POS EQUAL -1)
        message(FATAL_ERROR
            "Secure FDCAN .ioc contract missing: ${ECU_FDCAN_IOC_REQUIRED}")
    endif()
endforeach()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/fdcan.c"
     ECU_FDCAN_GENERATED_TEXT)
foreach(ECU_FDCAN_GENERATED_REQUIRED IN ITEMS
    "hfdcan1.Init.ClockDivider = FDCAN_CLOCK_DIV1;"
    "hfdcan1.Init.FrameFormat = FDCAN_FRAME_CLASSIC;"
    "hfdcan1.Init.Mode = FDCAN_MODE_NORMAL;"
    "hfdcan1.Init.AutoRetransmission = ENABLE;"
    "hfdcan1.Init.TransmitPause = ENABLE;"
    "hfdcan1.Init.NominalPrescaler = 4;"
    "hfdcan1.Init.NominalSyncJumpWidth = 4;"
    "hfdcan1.Init.NominalTimeSeg1 = 19;"
    "hfdcan1.Init.NominalTimeSeg2 = 5;"
    "hfdcan1.Init.StdFiltersNbr = 0;"
    "hfdcan1.Init.ExtFiltersNbr = 6;"
    "hfdcan2.Init.ClockDivider = FDCAN_CLOCK_DIV1;"
    "hfdcan2.Init.FrameFormat = FDCAN_FRAME_CLASSIC;"
    "hfdcan2.Init.Mode = FDCAN_MODE_NORMAL;"
    "hfdcan2.Init.AutoRetransmission = DISABLE;"
    "hfdcan2.Init.TransmitPause = ENABLE;"
    "hfdcan2.Init.NominalPrescaler = 4;"
    "hfdcan2.Init.NominalSyncJumpWidth = 4;"
    "hfdcan2.Init.NominalTimeSeg1 = 19;"
    "hfdcan2.Init.NominalTimeSeg2 = 5;"
    "hfdcan2.Init.StdFiltersNbr = 0;"
    "hfdcan2.Init.ExtFiltersNbr = 2;"
    "static uint32_t HAL_RCC_FDCAN_CLK_ENABLED=0;"
    "RCC_FDCANCLKSOURCE_HSE"
    "HAL_GPIO_ConfigPinAttributes(GPIOD, CAN1_RX_Pin|CAN1_TX_Pin, GPIO_PIN_SEC);"
    "HAL_GPIO_ConfigPinAttributes(GPIOB, CAN2_RX_Pin|CAN2_TX_Pin, GPIO_PIN_SEC);")
    string(FIND "${ECU_FDCAN_GENERATED_TEXT}"
           "${ECU_FDCAN_GENERATED_REQUIRED}"
           ECU_FDCAN_GENERATED_REQUIRED_POS)
    if(ECU_FDCAN_GENERATED_REQUIRED_POS EQUAL -1)
        message(FATAL_ERROR
            "Generated Secure FDCAN contract missing: ${ECU_FDCAN_GENERATED_REQUIRED}")
    endif()
endforeach()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/main.c" ECU_SECURE_MAIN_TEXT)
string(FIND "${ECU_SECURE_MAIN_TEXT}" "MX_FDCAN1_Init();"
       ECU_FDCAN1_INIT_POS)
string(FIND "${ECU_SECURE_MAIN_TEXT}" "MX_FDCAN2_Init();"
       ECU_FDCAN2_INIT_POS)
if(ECU_FDCAN1_INIT_POS EQUAL -1 OR ECU_FDCAN2_INIT_POS EQUAL -1 OR
   NOT ECU_FDCAN1_INIT_POS LESS ECU_FDCAN2_INIT_POS)
    message(FATAL_ERROR
        "Secure initialization must keep shared-clock FDCAN1 before FDCAN2")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc/partition_stm32h563xx.h"
     ECU_PARTITION_TEXT)
string(FIND "${ECU_PARTITION_TEXT}"
       "#define NVIC_INIT_ITNS1_VAL      0x00002000"
       ECU_FDCAN1_ITNS_POS)
string(FIND "${ECU_PARTITION_TEXT}"
       "#define NVIC_INIT_ITNS3_VAL      0x00000000"
       ECU_FDCAN2_ITNS_POS)
if(ECU_FDCAN1_ITNS_POS EQUAL -1)
    message(FATAL_ERROR
        "FDCAN1 IRQ39 must remain Secure (ITNS1 bit 7 clear)")
endif()
if(ECU_FDCAN2_ITNS_POS EQUAL -1)
    message(FATAL_ERROR
        "FDCAN2 IRQ109 must remain Secure (ITNS3 bit 13 clear)")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mx-generated.cmake"
     ECU_SECURE_MX_GENERATED_TEXT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../NonSecure/mx-generated.cmake"
     ECU_NONSECURE_MX_GENERATED_TEXT)
foreach(ECU_SECURE_FDCAN_BUILD_REQUIRED IN ITEMS
    "Core/Src/fdcan.c"
    "stm32h5xx_hal_fdcan.c")
    string(FIND "${ECU_SECURE_MX_GENERATED_TEXT}"
           "${ECU_SECURE_FDCAN_BUILD_REQUIRED}"
           ECU_SECURE_FDCAN_BUILD_REQUIRED_POS)
    if(ECU_SECURE_FDCAN_BUILD_REQUIRED_POS EQUAL -1)
        message(FATAL_ERROR
            "Secure CubeMX source list lost FDCAN: ${ECU_SECURE_FDCAN_BUILD_REQUIRED}")
    endif()
    string(FIND "${ECU_NONSECURE_MX_GENERATED_TEXT}"
           "${ECU_SECURE_FDCAN_BUILD_REQUIRED}"
           ECU_NONSECURE_FDCAN_BUILD_FORBIDDEN_POS)
    if(NOT ECU_NONSECURE_FDCAN_BUILD_FORBIDDEN_POS EQUAL -1)
        message(FATAL_ERROR
            "NonSecure CubeMX source list must not contain FDCAN: ${ECU_SECURE_FDCAN_BUILD_REQUIRED}")
    endif()
endforeach()


# In order to use CMake for cross-compiling
set(CMAKE_CXX_COMPILER_FORCED "true")

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    HAL_PKA_MODULE_ENABLED
    HAL_RNG_MODULE_ENABLED
    $<$<BOOL:${ECU_OEMIROT_LAYOUT}>:ECU_OEMIROT_LAYOUT=1>
    # system_stm32h5xx_s.c otherwise restores the reset vector at 0x0C000000,
    # while the signed Secure image vector follows its 0x400-byte MCUboot
    # header in the primary slot.
    $<$<BOOL:${ECU_OEMIROT_LAYOUT}>:VECT_TAB_OFFSET=0x00030400U>
    $<$<BOOL:${ECU_FACTORY_PROVISIONING}>:ECU_FACTORY_PROVISIONING=1>
)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    -Wextra
    -Wformat=2
    -Wundef
    -Werror
    -fno-common
)

target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/App/Linker/valve_config_flash_guard.ld
    $<$<BOOL:${ECU_OEMIROT_LAYOUT}>:-Wl,--defsym=ECU_OEMIROT_LAYOUT=1>
)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

add_custom_command(
    OUTPUT "${ECU_NSC_ABI_BASELINE_OBJECT}"
    COMMAND ${CMAKE_ASM_COMPILER} -mcpu=cortex-m33 -mthumb
            -I${CMAKE_CURRENT_SOURCE_DIR}/../Secure_nsclib -c
            "${ECU_NSC_ABI_BASELINE_SOURCE}"
            -o "${ECU_NSC_ABI_BASELINE_OBJECT}"
    COMMAND ${CMAKE_OBJCOPY}
            --remove-section=.text
            --remove-section=.data
            --remove-section=.bss
            --remove-section=.note.GNU-stack
            --remove-section=.ARM.attributes
            "${ECU_NSC_ABI_BASELINE_OBJECT}"
    DEPENDS "${ECU_NSC_ABI_BASELINE_SOURCE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Secure_nsclib/secure_nsc_abi_v2.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Secure_nsclib/secure_nsc_abi_v1.s"
    VERBATIM)
add_custom_target(ECU_NSC_ABI_BASELINE
    DEPENDS "${ECU_NSC_ABI_BASELINE_OBJECT}")
if(ECU_OEMIROT_LAYOUT)
    add_dependencies(${CMAKE_PROJECT_NAME} ECU_NSC_ABI_BASELINE)
    # --out-implib is emitted by the link command. Declare it as a byproduct so
    # Ninja relinks the Secure target if this private release input is missing.
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        BYPRODUCTS "${ECU_NSC_IMPORT_LIBRARY}"
        COMMAND ${CMAKE_COMMAND} -E true
        VERBATIM)
endif()

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Shared
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/atecc608.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/safety_service.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/security_crypto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/security_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/security_mcu_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/security_ota.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/security_ota_policy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/security_sha256.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/security_factory.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/secure_flash_guard.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/secure_timebase.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/software_i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/fdcan_callbacks.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/steering_control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/steering_can.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/vehicle_can.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/vehicle_j1939.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/valve_control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/valve_config_store.c
)

target_sources(STM32_Drivers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_pka.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_rng.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_rng_ex.c
)
target_compile_definitions(STM32_Drivers PRIVATE
    HAL_PKA_MODULE_ENABLED
    HAL_RNG_MODULE_ENABLED
    # CubeMX places system_stm32h5xx_s.c in this object library rather than in
    # ECU_S. Set the signed-slot vector offset on the target that actually
    # compiles SystemInit; definitions on ECU_S do not propagate here.
    $<$<BOOL:${ECU_OEMIROT_LAYOUT}>:VECT_TAB_OFFSET=0x00030400U>
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined libraries
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()
