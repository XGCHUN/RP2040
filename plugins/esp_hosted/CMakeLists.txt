# --------------------------------------------------------------------------
# ESP32 co-processor (slave) firmware source.
#
# The ESP32-C5/C6 runs Espressif's official ESP-Hosted "slave" firmware, built
# separately with ESP-IDF and flashed to the co-processor - it is NOT part of
# the RP2350 grblHAL build. Like the PIO USB library, the source is fetched on
# demand into managed_components/ (git-ignored) rather than vendored.
#
# This block only makes sure the source is present so you can build/flash it
# with ESP-IDF; it does not compile it here. It is best-effort: a missing clone
# is a warning (the RP2350 firmware still builds), not a fatal error.
#
# Pin the wire-compatible version this host targets (see esp_hosted_config.h:
# ESP_HOSTED_PROTOCOL 2.12.x). Override on the cmake command line if needed.
set(ESP_HOSTED_SLAVE_REPO "https://github.com/espressif/esp-hosted-mcu.git"
    CACHE STRING "ESP-Hosted slave firmware git repository")
set(ESP_HOSTED_SLAVE_REF  "main"
    CACHE STRING "ESP-Hosted slave firmware git ref (tag/branch/commit)")

set(_esp_hosted_slave_dir ${CMAKE_SOURCE_DIR}/managed_components/esp-hosted-mcu)

if(NOT EXISTS ${_esp_hosted_slave_dir}/.git)
    message(STATUS "ESP-Hosted slave firmware not found; cloning ${ESP_HOSTED_SLAVE_REF} into managed_components/esp-hosted-mcu ...")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --branch ${ESP_HOSTED_SLAVE_REF} --depth 1
                    ${ESP_HOSTED_SLAVE_REPO} ${_esp_hosted_slave_dir}
            RESULT_VARIABLE _esp_hosted_clone_rc
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT _esp_hosted_clone_rc EQUAL 0)
            message(WARNING
                "Could not clone ESP-Hosted slave firmware. Fetch it manually with:\n"
                "  git clone --branch ${ESP_HOSTED_SLAVE_REF} ${ESP_HOSTED_SLAVE_REPO} managed_components/esp-hosted-mcu\n"
                "Build/flash it separately with ESP-IDF for the ESP32-C5/C6 co-processor.")
        else()
            message(STATUS "ESP-Hosted slave firmware cloned into managed_components/esp-hosted-mcu")
        endif()
    else()
        message(WARNING "git not found; cannot fetch ESP-Hosted slave firmware into managed_components/esp-hosted-mcu")
    endif()
else()
    message(STATUS "ESP-Hosted slave firmware present at managed_components/esp-hosted-mcu")
endif()

add_library(esp_hosted_rp2350 INTERFACE)

target_sources(esp_hosted_rp2350 INTERFACE
 ${CMAKE_CURRENT_LIST_DIR}/esp_hosted.c
 ${CMAKE_CURRENT_LIST_DIR}/esp_hosted_rpc.c
 ${CMAKE_CURRENT_LIST_DIR}/esp_hosted_transport.c
)

if(ADD_BLUETOOTH)
 target_sources(esp_hosted_rp2350 INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}/esp_hosted_hci_transport.c
  ${CMAKE_CURRENT_LIST_DIR}/esp_hosted_ble.c
 )
 target_link_libraries(esp_hosted_rp2350 INTERFACE
  pico_btstack_ble
  pico_btstack_run_loop_async_context
  pico_async_context_poll
 )
endif()

target_include_directories(esp_hosted_rp2350 INTERFACE
 ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(esp_hosted_rp2350 INTERFACE
 hardware_gpio
 hardware_spi
 pico_multicore
 pico_stdlib
 pico_util
)
