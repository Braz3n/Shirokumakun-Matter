#
# Matter AC Controller for Seeed XIAO BLE (nRF52840)
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

project(matter-ac-controller)

# Enable GNU STD support and initialize the Matter Data Model.
include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)
include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/src/app/chip_data_model.cmake)

# Include paths
set(MATTER_COMMONS_SRC_DIR ${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/src)
target_include_directories(app PRIVATE
    src
    ${MATTER_COMMONS_SRC_DIR}
)

# Common Matter sources (cherry-picked to avoid DK library dependency)
target_sources(app PRIVATE
    ${MATTER_COMMONS_SRC_DIR}/app/task_executor.cpp
    ${MATTER_COMMONS_SRC_DIR}/app/matter_init.cpp
    ${MATTER_COMMONS_SRC_DIR}/app/matter_event_handler.cpp
)

# Application sources
target_sources(app PRIVATE
    src/main.cpp
    src/app_task.cpp
    src/zcl_callbacks.cpp
    src/ir_driver.cpp
    src/ir_protocol.cpp
    src/scd40_manager.cpp
)

chip_configure_data_model(app
    INCLUDE_SERVER
    BYPASS_IDL
    GEN_DIR src/zap-generated
    ZAP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/ac_controller.zap
)
