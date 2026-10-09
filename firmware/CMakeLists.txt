cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.5.19")   # shown on the OTA screen and by GET /version; bump for every release
project(vandeck)

# BreezyBox's terminal (valdanylchuk/breezy_term) includes driver/usb_serial_jtag.h but only REQUIRES the legacy
# 'driver' component; on IDF 6 that header lives in esp_driver_usb_serial_jtag.
idf_component_get_property(breezy_term_lib valdanylchuk__breezy_term COMPONENT_LIB)
target_link_libraries(${breezy_term_lib} PRIVATE idf::esp_driver_usb_serial_jtag)
# GCC 15 flags BreezyBox's deliberate truncating string copies; keep them warnings in that component only.
idf_component_get_property(breezybox_lib valdanylchuk__breezybox COMPONENT_LIB)
target_compile_options(${breezybox_lib} PRIVATE -Wno-error=stringop-truncation -Wno-error=format-truncation)
