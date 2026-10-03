cmake_minimum_required(VERSION 3.16)
project(lawrec_demo_linux LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(SDK "$ENV{K230_SDK_ROOT}")
set(CDK "${SDK}/src/common/cdk/user")
set(MPP "${SDK}/src/big/mpp")
set(LIVE "${CDK}/thirdparty/live")
set(LVGL_ROOT "$ENV{LVGL_ROOT}")
include("${CMAKE_CURRENT_SOURCE_DIR}/../tools/mp4.cmake")
add_executable(media_service media/main.cpp media/vision_client.cpp
    media/source.cpp media/source_frames.cpp media/live_source.cpp media/rtsp_stream.cpp
    media/rtsp.cpp media/recorder.cpp media/mp4_sink.cpp media/metrics.cpp ../common/frame.cpp ../common/frame_queue.cpp)
target_compile_definitions(media_service PRIVATE NO_OPENSSL=1)
target_compile_options(media_service PRIVATE -Wall -Wextra)
target_include_directories(media_service PRIVATE ../common
    "${MPP}/include" "${MPP}/include/comm" "${MPP}/userapps/api"
    "${CDK}/mapi/include" "${CDK}/mapi/include/api" "${CDK}/mapi/include/comm"
    "${CDK}/component/ipcmsg/include" "${CDK}/component/datafifo/include")
target_include_directories(media_service PRIVATE "${LIVE}/liveMedia/include"
    "${LIVE}/groupsock/include" "${LIVE}/UsageEnvironment/include" "${LIVE}/BasicUsageEnvironment/include")
target_link_directories(media_service PRIVATE "${CDK}/mapi/mediaclient/lib"
    "${CDK}/component/ipcmsg/host/lib" "${CDK}/component/datafifo/slave/lib")
target_link_directories(media_service PRIVATE "${LIVE}/liveMedia" "${LIVE}/groupsock"
    "${LIVE}/UsageEnvironment" "${LIVE}/BasicUsageEnvironment")
target_link_libraries(media_service PRIVATE -Wl,--start-group mapi ipcmsg datafifo
    liveMedia groupsock BasicUsageEnvironment UsageEnvironment sdk_mp4 pthread -Wl,--end-group)
set(PORT "${CMAKE_CURRENT_SOURCE_DIR}/../compat/lvgl_port")
file(GLOB_RECURSE LVGL_SOURCES "${LVGL_ROOT}/src/*.c")
if(NOT LVGL_SOURCES)
    message(FATAL_ERROR "SDK LVGL 8.3.1 sources required, set LVGL_ROOT")
endif()
add_library(lvgl SHARED ${LVGL_SOURCES} "${PORT}/lawrec_font_cn_16.c")
target_include_directories(lvgl PUBLIC "${LVGL_ROOT}" "${PORT}")
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE=1 LV_LVGL_H_INCLUDE_SIMPLE=1)
target_link_libraries(lvgl PRIVATE m)
add_library(lv_drivers SHARED "${PORT}/k230/lv_port_disp.cpp"
    "${PORT}/k230/lv_port_indev.c" "${PORT}/k230/buf_mgt.cpp")
target_compile_definitions(lv_drivers PRIVATE CONFIG_BOARD_K230_CANMV_LCKFB=1)
target_include_directories(lv_drivers PRIVATE "${CMAKE_SYSROOT}/usr/include/libdrm")
target_link_libraries(lv_drivers PRIVATE lvgl disp pthread drm)
add_executable(demo_ui ui/main.cpp ui/page.cpp ../common/socket.cpp)
target_compile_options(demo_ui PRIVATE -Wall -Wextra)
target_include_directories(demo_ui PRIVATE ../common)
target_link_libraries(demo_ui PRIVATE lv_drivers lvgl pthread)
add_executable(democtl ui/ctl.cpp ../common/socket.cpp)
target_compile_options(democtl PRIVATE -Wall -Wextra)
target_include_directories(democtl PRIVATE ../common)
set_target_properties(media_service demo_ui democtl PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN/../lib")
set_target_properties(lvgl lv_drivers PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
