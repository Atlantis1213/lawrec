cmake_minimum_required(VERSION 3.10)
project(lawrec_ui_preview C)
set(ROOT "$ENV{LAWREC_PROJECT_ROOT}")
set(LVGL "$ENV{K230_SDK_ROOT}/output/k230_canmv_lckfb_defconfig/little/buildroot-ext/build/lawrec/thirdlib/lvgl")
if(NOT EXISTS "${LVGL}/lvgl.h")
    message(FATAL_ERROR "Run the Docker little build once to stage LVGL")
endif()
file(GLOB_RECURSE LVGL_SRC "${LVGL}/src/*.c")
add_library(preview-lvgl STATIC ${LVGL_SRC})
target_include_directories(preview-lvgl PUBLIC "${LVGL}")
target_compile_definitions(preview-lvgl PUBLIC LV_CONF_PATH=${ROOT}/little/src/ui/lvgl_port/lv_conf.h LV_LVGL_H_INCLUDE_SIMPLE)
set(UI "${ROOT}/little/src/ui/src")
file(GLOB SETTINGS_HELPERS "${UI}/settings_ui.c")
add_executable(ui-preview main.c ${SETTINGS_HELPERS}
    ${UI}/scr_settings.c ${UI}/scr_network.c ${UI}/scr_ipv4.c
    ${UI}/scr_storage.c ${UI}/scr_time.c ${UI}/scr_maintenance.c
    ${UI}/scr_files.c ${UI}/scr_playback.c
    ${UI}/lawrec_font_cn_16.c ${UI}/lawrec_font_cn_20.c)
target_include_directories(ui-preview PRIVATE "${UI}" "${ROOT}/little/src/common" "${ROOT}/little/src/control/include")
target_link_libraries(ui-preview PRIVATE preview-lvgl m)
