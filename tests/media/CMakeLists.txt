cmake_minimum_required(VERSION 3.16)
project(media_offline LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SDK "$ENV{K230_SDK_ROOT}")
set(CDK "${SDK}/src/common/cdk/user")
set(MPP "${SDK}/src/big/mpp")
include("${ROOT}/tools/mp4.cmake")
set(LIVE "${CDK}/thirdparty/live")
add_executable(media_test media_test.cpp "${ROOT}/little/media/mp4_sink.cpp"
    "${ROOT}/little/media/recorder.cpp" "${ROOT}/little/media/rtsp.cpp"
    "${ROOT}/little/media/live_source.cpp" "${ROOT}/little/media/rtsp_stream.cpp"
    "${ROOT}/common/frame.cpp" "${ROOT}/common/frame_queue.cpp")
target_compile_options(media_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(media_test PRIVATE "${ROOT}/common" "${ROOT}/little/media"
    "${MPP}/include" "${MPP}/include/comm" "${MPP}/userapps/api"
    "${CDK}/mapi/include" "${CDK}/mapi/include/api" "${CDK}/mapi/include/comm")
target_include_directories(media_test SYSTEM PRIVATE "${LIVE}/liveMedia/include" "${LIVE}/groupsock/include"
    "${LIVE}/UsageEnvironment/include" "${LIVE}/BasicUsageEnvironment/include")
target_link_libraries(media_test PRIVATE sdk_mp4 -Wl,--start-group
    "${ROOT}/out/tests/rtsp/libliveMedia.a" "${ROOT}/out/tests/rtsp/libgroupsock.a"
    "${ROOT}/out/tests/rtsp/libBasicUsageEnvironment.a" "${ROOT}/out/tests/rtsp/libUsageEnvironment.a"
    -Wl,--end-group pthread)
target_compile_definitions(media_test PRIVATE NO_OPENSSL=1)
