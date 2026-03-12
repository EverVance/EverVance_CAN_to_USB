cmake_minimum_required(VERSION 3.20)

project(VBA_CAN C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for IDE tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VBA_CAN_BUILD_BOOTLOADER "Build the Bootloader target." ON)
option(VBA_CAN_BUILD_CAN_APP "Build the CAN_APP target." ON)

function(collect_include_dirs BASE_DIR OUT_VAR)
    file(GLOB_RECURSE HEADER_FILES CONFIGURE_DEPENDS
        ${BASE_DIR}/*.h
        ${BASE_DIR}/*.hpp
        ${BASE_DIR}/*.hh
        ${BASE_DIR}/*.hxx
    )

    set(INCLUDE_DIRS ${BASE_DIR})
    foreach(HEADER_FILE ${HEADER_FILES})
        get_filename_component(HEADER_DIR ${HEADER_FILE} DIRECTORY)
        list(APPEND INCLUDE_DIRS ${HEADER_DIR})
    endforeach()
    list(REMOVE_DUPLICATES INCLUDE_DIRS)

    set(${OUT_VAR} "${INCLUDE_DIRS}" PARENT_SCOPE)
endfunction()

set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(HARDWARE_DIR ${PROJECT_ROOT}/Hardware)
set(BOOTLOADER_DIR ${PROJECT_ROOT}/Bootloader)
set(CAN_APP_DIR ${PROJECT_ROOT}/CAN_APP)
set(VBA_CAN_OUTPUT_ROOT ${PROJECT_ROOT}/out CACHE PATH "Workspace output root.")
set(VBA_CAN_ARTIFACTS_DIR ${VBA_CAN_OUTPUT_ROOT}/artifacts CACHE PATH "Artifact output root.")

set(ARM_CPU_OPTIONS
    -mcpu=cortex-m7
    -mthumb
    -mfpu=fpv5-d16
    -mfloat-abi=hard
)

file(GLOB_RECURSE HARDWARE_SOURCES CONFIGURE_DEPENDS
    ${HARDWARE_DIR}/*.c
    ${HARDWARE_DIR}/*.cpp
    ${HARDWARE_DIR}/*.s
    ${HARDWARE_DIR}/*.S
)

collect_include_dirs(${HARDWARE_DIR} HARDWARE_INCLUDE_DIRS)

if(HARDWARE_SOURCES)
    add_library(hardware_common STATIC ${HARDWARE_SOURCES})
    target_include_directories(hardware_common PUBLIC ${HARDWARE_INCLUDE_DIRS})
    target_compile_definitions(hardware_common PUBLIC
        USE_HAL_DRIVER
        CPU_MIMXRT1062CVJ5B
        CPU_MIMXRT1062CVJ5B_cm7
        MCUXPRESSO_SDK
        XIP_EXTERNAL_FLASH=1
        XIP_BOOT_HEADER_ENABLE=1
        SDK_DEBUGCONSOLE=1
        CR_INTEGER_PRINTF
        PRINTF_FLOAT_ENABLE=0
        __MCUXPRESSO
        __USE_CMSIS
    )
    target_compile_options(hardware_common PUBLIC ${ARM_CPU_OPTIONS})
    target_compile_options(hardware_common PRIVATE
        -Wall
        -Wextra
    )
else()
    message(STATUS "No C/C++/ASM sources found in Hardware; hardware_common is INTERFACE only.")
    add_library(hardware_common INTERFACE)
    target_include_directories(hardware_common INTERFACE ${HARDWARE_INCLUDE_DIRS})
    target_compile_definitions(hardware_common INTERFACE
        USE_HAL_DRIVER
        CPU_MIMXRT1062CVJ5B
        CPU_MIMXRT1062CVJ5B_cm7
        MCUXPRESSO_SDK
        XIP_EXTERNAL_FLASH=1
        XIP_BOOT_HEADER_ENABLE=1
        SDK_DEBUGCONSOLE=1
        CR_INTEGER_PRINTF
        PRINTF_FLOAT_ENABLE=0
        __MCUXPRESSO
        __USE_CMSIS
    )
    target_compile_options(hardware_common INTERFACE ${ARM_CPU_OPTIONS})
endif()

if(NOT VBA_CAN_BUILD_BOOTLOADER AND NOT VBA_CAN_BUILD_CAN_APP)
    message(FATAL_ERROR "At least one firmware target must be enabled.")
endif()

if(VBA_CAN_BUILD_BOOTLOADER)
    add_subdirectory(Bootloader)
endif()

if(VBA_CAN_BUILD_CAN_APP)
    add_subdirectory(CAN_APP)
endif()
