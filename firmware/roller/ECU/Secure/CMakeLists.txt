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
set(STM32_LINKER_OPTION  -Wl,--cmse-implib\ -Wl,--out-implib="${CMAKE_CURRENT_SOURCE_DIR}/../Secure_nsclib/secure_nsclib.o")

# Set the project name
set(CMAKE_PROJECT_NAME ECU_S)
project(${CMAKE_PROJECT_NAME})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Include mx-generated
include("mx-generated.cmake")


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
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/security_sha256.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/security_factory.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/software_i2c.c
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
