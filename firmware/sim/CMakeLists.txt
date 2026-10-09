# Desktop simulator: the real UI (main/ui) on an 800x480 e-paper preview. See README.md.
cmake_minimum_required(VERSION 3.16)
project(vandeck_sim C)
set(CMAKE_C_STANDARD 11)

find_package(SDL2 REQUIRED)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LVGL ${FW}/managed_components/lvgl__lvgl)
if(NOT EXISTS ${LVGL}/lvgl.h)
    message(FATAL_ERROR "LVGL not found in managed_components: run 'idf.py reconfigure' in firmware/ once")
endif()

file(GLOB_RECURSE LVGL_SRC ${LVGL}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC ${LVGL} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)

file(GLOB UI_SRC ${FW}/main/ui/*.c ${FW}/main/fonts/*.c)
add_executable(vandeck_sim
    sim_main.c sim_services.c
    ${UI_SRC}
    ${FW}/main/app/model.c
    ${FW}/main/app/sim.c
    ${FW}/main/io/truma_errors.c)
target_include_directories(vandeck_sim PRIVATE stubs ${FW}/main ${FW}/main/ui ${FW}/main/app ${FW}/main/io)
target_compile_options(vandeck_sim PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/stubs/sdkconfig.h -Wno-unused-function)
target_link_libraries(vandeck_sim PRIVATE lvgl SDL2::SDL2 m)
