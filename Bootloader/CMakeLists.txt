cmake_minimum_required(VERSION 3.20)

file(GLOB_RECURSE BOOTLOADER_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.s
    ${CMAKE_CURRENT_SOURCE_DIR}/*.S
)

add_executable(bootloader.elf
    ${BOOTLOADER_SOURCES}
)

set(BOOTLOADER_OUTPUT_DIR ${VBA_CAN_ARTIFACTS_DIR}/Bootloader)

collect_include_dirs(${CMAKE_CURRENT_SOURCE_DIR} BOOTLOADER_INCLUDE_DIRS)

target_include_directories(bootloader.elf PRIVATE
    ${BOOTLOADER_INCLUDE_DIRS}
)

target_link_libraries(bootloader.elf PRIVATE
    hardware_common
)

target_compile_definitions(bootloader.elf PRIVATE
    BOOTLOADER
    SDK_OS_BAREMETAL
)

target_compile_options(bootloader.elf PRIVATE
    -Wall
    -Wextra
)

set_target_properties(bootloader.elf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BOOTLOADER_OUTPUT_DIR}
)

target_link_options(bootloader.elf PRIVATE
    ${ARM_CPU_OPTIONS}
    -specs=nano.specs
    -specs=nosys.specs
    -Wl,--gc-sections
    -Wl,-Map,${BOOTLOADER_OUTPUT_DIR}/bootloader.map
)

set(BOOTLOADER_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/Bootloader_Debug.ld)
if(EXISTS ${BOOTLOADER_LINKER_SCRIPT})
    target_link_options(bootloader.elf PRIVATE
        -T${BOOTLOADER_LINKER_SCRIPT}
    )
else()
    message(WARNING "Bootloader linker script not found: ${BOOTLOADER_LINKER_SCRIPT}")
endif()

if(CMAKE_OBJCOPY)
    add_custom_command(TARGET bootloader.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:bootloader.elf>
            ${BOOTLOADER_OUTPUT_DIR}/bootloader.bin
        BYPRODUCTS ${BOOTLOADER_OUTPUT_DIR}/bootloader.bin
        COMMENT "Generating Bootloader binary"
    )
endif()
