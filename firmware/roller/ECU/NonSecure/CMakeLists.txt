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

set(ECU_LWIP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/LwIP)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Compiler options
set(STM32_MCU_FLAGS  "-mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard " )

#Linker options
set(STM32_LINKER_SCRIPT STM32H563xx_FLASH_ns.ld)
set(STM32_LINKER_OPTION  )

# Set the project name
set(CMAKE_PROJECT_NAME ECU_NS)
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
)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    -Wextra
    -Wformat=2
    -Wundef
    -Werror
    -fno-common
)

# ETH DMA is intentionally granted non-privileged access only to the first
# 64 KiB of SRAM3.  Keep every static NonSecure object (including all LwIP
# pools/descriptors) inside that reviewed window so future growth fails at link
# time instead of silently breaking Ethernet on hardware.
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/App/Linker/eth_dma_sram3_guard.ld
)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Network
    ${ECU_LWIP_ROOT}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/BSP/Components/lan8742
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/ecu_app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/ecu_protocol.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/ecu_data_model.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/ecu_network.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/j1939.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/speed_sensor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Network/ethernetif.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/BSP/Components/lan8742/lan8742.c

    ${ECU_LWIP_ROOT}/src/core/init.c
    ${ECU_LWIP_ROOT}/src/core/def.c
    ${ECU_LWIP_ROOT}/src/core/dns.c
    ${ECU_LWIP_ROOT}/src/core/inet_chksum.c
    ${ECU_LWIP_ROOT}/src/core/ip.c
    ${ECU_LWIP_ROOT}/src/core/mem.c
    ${ECU_LWIP_ROOT}/src/core/memp.c
    ${ECU_LWIP_ROOT}/src/core/netif.c
    ${ECU_LWIP_ROOT}/src/core/pbuf.c
    ${ECU_LWIP_ROOT}/src/core/raw.c
    ${ECU_LWIP_ROOT}/src/core/stats.c
    ${ECU_LWIP_ROOT}/src/core/sys.c
    ${ECU_LWIP_ROOT}/src/core/timeouts.c
    ${ECU_LWIP_ROOT}/src/core/udp.c
    ${ECU_LWIP_ROOT}/src/core/ipv4/acd.c
    ${ECU_LWIP_ROOT}/src/core/ipv4/autoip.c
    ${ECU_LWIP_ROOT}/src/core/ipv4/dhcp.c
    ${ECU_LWIP_ROOT}/src/core/ipv4/etharp.c
    ${ECU_LWIP_ROOT}/src/core/ipv4/icmp.c
    ${ECU_LWIP_ROOT}/src/core/ipv4/igmp.c
    ${ECU_LWIP_ROOT}/src/core/ipv4/ip4.c
    ${ECU_LWIP_ROOT}/src/core/ipv4/ip4_addr.c
    ${ECU_LWIP_ROOT}/src/core/ipv4/ip4_frag.c
    ${ECU_LWIP_ROOT}/src/netif/ethernet.c
)

# The unmodified ST LAN8742 component intentionally keeps an interrupt-mask
# parameter for API compatibility although this PHY cannot use it.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/BSP/Components/lan8742/lan8742.c
    PROPERTIES COMPILE_OPTIONS -Wno-unused-parameter
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


