cmake_minimum_required(VERSION 3.16)
project(rtsp_offline LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SDK "$ENV{K230_SDK_ROOT}")
set(CDK "${SDK}/src/common/cdk/user")
set(MPP "${SDK}/src/big/mpp")
set(LIVE "${CDK}/thirdparty/live")
# Compile the unmodified SDK library natively; its installed archives are RISC-V.
add_library(live_options INTERFACE)
target_compile_definitions(live_options INTERFACE NO_OPENSSL=1 SOCKLEN_T=socklen_t)
target_include_directories(live_options SYSTEM INTERFACE "${LIVE}/liveMedia/include"
    "${LIVE}/groupsock/include" "${LIVE}/UsageEnvironment/include" "${LIVE}/BasicUsageEnvironment/include")
foreach(component liveMedia groupsock BasicUsageEnvironment UsageEnvironment)
    file(GLOB sources "${LIVE}/${component}/*.cpp" "${LIVE}/${component}/*.c")
    add_library(${component} STATIC ${sources})
    target_link_libraries(${component} PUBLIC live_options)
endforeach()
add_executable(rtsp_test rtsp_test.cpp "${ROOT}/little/media/live_source.cpp"
    "${ROOT}/little/media/rtsp_stream.cpp" "${ROOT}/little/media/rtsp.cpp"
    "${ROOT}/common/frame.cpp" "${ROOT}/common/frame_queue.cpp")
target_compile_options(rtsp_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(rtsp_test PRIVATE "${ROOT}/common" "${ROOT}/little/media"
    "${MPP}/include" "${MPP}/include/comm" "${MPP}/userapps/api"
    "${CDK}/mapi/include" "${CDK}/mapi/include/api" "${CDK}/mapi/include/comm")
target_link_libraries(rtsp_test PRIVATE -Wl,--start-group
    liveMedia groupsock BasicUsageEnvironment UsageEnvironment -Wl,--end-group pthread)
