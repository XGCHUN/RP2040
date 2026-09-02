# grblHAL PIO USB host wrapper.
#
# Included from the top-level CMakeLists.txt when ADD_PIO_USB_HOST is ON, after
# the grblHAL target has been created (this file calls target_sources() on it).
#
# This file owns the whole PIO USB integration:
#   - pulls in the third-party Pico-PIO-USB library (pico_pio_usb),
#   - adds our host bring-up source to the grblHAL target,
#   - links the library + TinyUSB host stack,
#   - enables the host code paths (PIO_USB_HOST_ENABLE) used by tusb_config.h
#     and driver.c.
#
# The third-party library is NOT vendored in git; clone it on demand into
# managed_components/ (git-ignored):
#   git clone https://github.com/sekigon-gonnoc/Pico-PIO-USB.git \
#       managed_components/Pico-PIO-USB
# Note: add_subdirectory() for it must run after pico_sdk_init(), which is
# already the case since the top-level file includes us after that.

set(PICO_PIO_USB_DIR ${CMAKE_CURRENT_LIST_DIR}/../managed_components/Pico-PIO-USB)
if(NOT EXISTS ${PICO_PIO_USB_DIR}/CMakeLists.txt)
    message(FATAL_ERROR
        "ADD_PIO_USB_HOST is ON but Pico-PIO-USB is missing. Clone it with:\n"
        "  git clone https://github.com/sekigon-gonnoc/Pico-PIO-USB.git managed_components/Pico-PIO-USB")
endif()
add_subdirectory(${PICO_PIO_USB_DIR} pico_pio_usb)

target_sources(grblHAL PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/pio_usb_host.c    # host core: startup, task, msg ring, mount
    ${CMAKE_CURRENT_LIST_DIR}/pio_usb_hid.c     # HID class  (CFG_TUH_HID)
    ${CMAKE_CURRENT_LIST_DIR}/pio_usb_msc.c     # MSC class  (CFG_TUH_MSC)
    ${CMAKE_CURRENT_LIST_DIR}/pio_usb_uvc.c     # UVC camera (PIO_USB_UVC_ENABLE)
)

target_include_directories(grblHAL PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(grblHAL PRIVATE
    pico_pio_usb
    tinyusb_host
)

target_compile_definitions(grblHAL PUBLIC PIO_USB_HOST_ENABLE=1)
