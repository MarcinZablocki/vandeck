cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.7.0")   # shown on the OTA screen and by GET /version; bump for every release
# lwIP derives PBUF_POOL_BUFSIZE from TCP_MSS, and sdkconfig keeps the MSS at 536 for the weak link (612-byte
# buffers). CHIP sizes its packet buffers from it and Matter needs ~1280 bytes, so set it on its own, build-wide so
# lwIP and CHIP agree. IDF's lwIP mallocs pbufs (MEMP_MEM_MALLOC), so there's no pool that grows with it.
idf_build_set_property(COMPILE_DEFINITIONS "PBUF_POOL_BUFSIZE=1536" APPEND)
# What Apple Home shows as Manufacturer and Model (CHIP's defaults are TEST_VENDOR / TEST_PRODUCT).
idf_build_set_property(COMPILE_DEFINITIONS "CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME=\"VanDeck\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME=\"VanDeck panel\"" APPEND)
project(vandeck)

# BreezyBox's terminal (valdanylchuk/breezy_term) includes driver/usb_serial_jtag.h but only REQUIRES the legacy
# 'driver' component; on IDF 6 that header lives in esp_driver_usb_serial_jtag.
idf_component_get_property(breezy_term_lib valdanylchuk__breezy_term COMPONENT_LIB)
target_link_libraries(${breezy_term_lib} PRIVATE idf::esp_driver_usb_serial_jtag)
# GCC 15 flags BreezyBox's deliberate truncating string copies; keep them warnings in that component only.
idf_component_get_property(breezybox_lib valdanylchuk__breezybox COMPONENT_LIB)
target_compile_options(${breezybox_lib} PRIVATE -Wno-error=stringop-truncation -Wno-error=format-truncation)
# esp_matter 1.6.0 targets IDF 5.5; IDF 6's GCC flags two truncation warnings in connectedhomeip as errors.
idf_component_get_property(matter_lib espressif__esp_matter COMPONENT_LIB)
target_compile_options(${matter_lib} PRIVATE -Wno-error=stringop-truncation -Wno-error=format-truncation)
