# ACSI2TNFS firmware - Raspberry Pi Pico (RP2040, non-W)

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(acsi2tnfs C CXX ASM)

pico_sdk_init()

# The one version number of the project (firmware, Atari driver, info texts)
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/version.txt ACSI2TNFS_VERSION LIMIT_COUNT 1)
string(STRIP "${ACSI2TNFS_VERSION}" ACSI2TNFS_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/version.txt)

# Atari side: atari/mkdisk.py assembles boot code and driver
# (with the version from version.txt) and writes the built-in disk image
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB ATARI_SOURCES ${CMAKE_CURRENT_LIST_DIR}/atari/*.s ${CMAKE_CURRENT_LIST_DIR}/atari/*.inc
                        ${CMAKE_CURRENT_LIST_DIR}/atari/files/*)
list(FILTER ATARI_SOURCES EXCLUDE REGEX "version\\.inc$")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_LIST_DIR}/disk_seed.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/atari/mkdisk.py
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/version.txt ${CMAKE_CURRENT_LIST_DIR}/atari/mkdisk.py ${ATARI_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/atari
    COMMENT "Building the Atari boot code, driver and disk image"
    VERBATIM)
add_custom_target(atari_disk DEPENDS ${CMAKE_CURRENT_LIST_DIR}/disk_seed.h)

add_executable(acsi2tnfs acsi2tnfs.c acsi_core1.c net.c config.c)
add_dependencies(acsi2tnfs atari_disk)

# Wi-Fi boards (pico_w, pico2_w): lwIP + CYW43, background mode on core0
if (PICO_CYW43_SUPPORTED)
    target_link_libraries(acsi2tnfs pico_cyw43_arch_lwip_threadsafe_background)
endif()

pico_generate_pio_header(acsi2tnfs ${CMAKE_CURRENT_LIST_DIR}/acsi_bus.pio)

target_compile_definitions(acsi2tnfs PRIVATE
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=1
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    PICO_STACK_SIZE=0x1000
    FW_VERSION_STR="${ACSI2TNFS_VERSION}"
)

pico_set_program_name(acsi2tnfs "acsi2tnfs")
pico_set_program_version(acsi2tnfs "${ACSI2TNFS_VERSION}")

pico_enable_stdio_uart(acsi2tnfs 0)
pico_enable_stdio_usb(acsi2tnfs 1)

target_link_libraries(acsi2tnfs
    pico_stdlib
    pico_multicore
    pico_flash
    hardware_pio
    hardware_flash
    hardware_dma
)

target_include_directories(acsi2tnfs PRIVATE ${CMAKE_CURRENT_LIST_DIR})

pico_add_extra_outputs(acsi2tnfs)
