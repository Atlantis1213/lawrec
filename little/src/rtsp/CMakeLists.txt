get_filename_component(FOLDER_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

set(EXE_TARGET lawrec_rtsp)

if(LAWREC_SDK_ROOT STREQUAL "")
    message(FATAL_ERROR "LAWREC_SDK_ROOT is required")
endif()

set(LAWREC_CDK_ROOT "${LAWREC_SDK_ROOT}/src/common/cdk/user")
set(LAWREC_MPP_API_ROOT "${LAWREC_SDK_ROOT}/src/big/mpp/userapps/api")
set(LAWREC_MAPI_ROOT "${LAWREC_CDK_ROOT}/mapi")
set(LAWREC_IPCMSG_ROOT "${LAWREC_CDK_ROOT}/component/ipcmsg")
set(LAWREC_DATAFIFO_ROOT "${LAWREC_CDK_ROOT}/component/datafifo")
set(LAWREC_LIVE_ROOT "${LAWREC_CDK_ROOT}/thirdparty/live")

file(GLOB RTSP_SRC
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")

add_executable(${EXE_TARGET} ${RTSP_SRC})

target_compile_definitions(${EXE_TARGET}
    PRIVATE NO_OPENSSL=1)

target_include_directories(${EXE_TARGET}
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include"
    PRIVATE "${LAWREC_IPCMSG_ROOT}/include"
    PRIVATE "${LAWREC_MAPI_ROOT}/include"
    PRIVATE "${LAWREC_MAPI_ROOT}/include/api"
    PRIVATE "${LAWREC_MAPI_ROOT}/include/comm"
    PRIVATE "${LAWREC_LIVE_ROOT}/BasicUsageEnvironment/include"
    PRIVATE "${LAWREC_LIVE_ROOT}/groupsock/include"
    PRIVATE "${LAWREC_LIVE_ROOT}/liveMedia/include"
    PRIVATE "${LAWREC_LIVE_ROOT}/UsageEnvironment/include"
    PRIVATE "${LAWREC_MPP_API_ROOT}"
    PRIVATE "${LAWREC_MPP_INC_DIR}"
    PRIVATE "${LAWREC_MPP_COMM_INC_DIR}")

target_link_directories(${EXE_TARGET}
    PRIVATE "${LAWREC_IPCMSG_ROOT}/host/lib"
    PRIVATE "${LAWREC_MAPI_ROOT}/mediaclient/lib"
    PRIVATE "${LAWREC_DATAFIFO_ROOT}/host/lib"
    PRIVATE "${LAWREC_LIVE_ROOT}/BasicUsageEnvironment"
    PRIVATE "${LAWREC_LIVE_ROOT}/groupsock"
    PRIVATE "${LAWREC_LIVE_ROOT}/liveMedia"
    PRIVATE "${LAWREC_LIVE_ROOT}/UsageEnvironment")

target_link_libraries(${EXE_TARGET}
    PRIVATE ipcmsg
    PRIVATE pthread
    PRIVATE mapi
    PRIVATE datafifo
    PRIVATE liveMedia
    PRIVATE UsageEnvironment
    PRIVATE BasicUsageEnvironment
    PRIVATE groupsock)

install(TARGETS ${EXE_TARGET}
    RUNTIME DESTINATION rtsp)
