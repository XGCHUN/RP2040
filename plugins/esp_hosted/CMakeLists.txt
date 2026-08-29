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
