cmake_minimum_required(VERSION 3.20)

file(GLOB_RECURSE CAN_APP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.s
    ${CMAKE_CURRENT_SOURCE_DIR}/*.S
)

list(REMOVE_ITEM CAN_APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/component/osa/fsl_os_abstraction_bm.c
)

add_executable(can_app.elf
    ${CAN_APP_SOURCES}
)

set(CAN_APP_OUTPUT_DIR ${VBA_CAN_ARTIFACTS_DIR}/CAN_APP)

collect_include_dirs(${CMAKE_CURRENT_SOURCE_DIR} CAN_APP_INCLUDE_DIRS)

target_include_directories(can_app.elf PRIVATE
    ${CAN_APP_INCLUDE_DIRS}
)

target_link_libraries(can_app.elf PRIVATE
    hardware_common
)

target_compile_definitions(can_app.elf PRIVATE
    CAN_APP
    SDK_OS_FREE_RTOS
    CAN_LOOPBACK_MODE=0
)

target_compile_options(can_app.elf PRIVATE
    -Wall
    -Wextra
)

set_target_properties(can_app.elf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CAN_APP_OUTPUT_DIR}
)

target_link_options(can_app.elf PRIVATE
    ${ARM_CPU_OPTIONS}
    -specs=nano.specs
    -specs=nosys.specs
    -Wl,--gc-sections
    -Wl,-Map,${CAN_APP_OUTPUT_DIR}/can_app.map
)

set(CAN_APP_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/CAN_APP_Debug.ld)
if(EXISTS ${CAN_APP_LINKER_SCRIPT})
    target_link_options(can_app.elf PRIVATE
        -T${CAN_APP_LINKER_SCRIPT}
    )
else()
    message(WARNING "CAN_APP linker script not found: ${CAN_APP_LINKER_SCRIPT}")
endif()

if(CMAKE_OBJCOPY)
    add_custom_command(TARGET can_app.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:can_app.elf>
            ${CAN_APP_OUTPUT_DIR}/can_app.bin
        BYPRODUCTS ${CAN_APP_OUTPUT_DIR}/can_app.bin
        COMMENT "Generating CAN_APP binary"
    )
endif()
