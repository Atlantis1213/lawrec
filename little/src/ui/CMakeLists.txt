get_filename_component(FOLDER_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

if(LAWREC_BOARD_LCKFB)
    add_compile_definitions(CONFIG_BOARD_K230_CANMV_LCKFB=1)
endif()

# build lvgl share library
add_subdirectory(lvgl_port)

# config exe_target
set(EXE_TARGET ${FOLDER_NAME})

aux_source_directory(src EXE_SRC)
file(GLOB CONTROL_SRC
    "${CMAKE_CURRENT_LIST_DIR}/../control/src/*.cpp")
file(GLOB RECORD_SRC
    "${CMAKE_CURRENT_LIST_DIR}/../record/src/*.cpp")
file(GLOB RTSP_SRC
    "${CMAKE_CURRENT_LIST_DIR}/../rtsp/main.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../rtsp/src/*.cpp")

add_executable(${EXE_TARGET}
    ../common/lawrec_settings.cpp
    ../common/lawrec_time.cpp
    ../common/lawrec_network.cpp
    ../common/lawrec_process.cpp
    ../common/lawrec_wifi_transaction.cpp
    ../common/lawrec_storage.cpp
    ../common/lawrec_media.cpp
    ../common/lawrec_encoder.cpp
    ../common/lawrec_audio.cpp
    ../playback/lawrec_playback.cpp
    ${EXE_SRC}
    ${CONTROL_SRC}
    ${RECORD_SRC}
    ${RTSP_SRC})

set(IPCMSG_PATH $ENV{CDK_SRC_DIR}/user/component/ipcmsg)
set(MPP_PATH $ENV{MPP_SRC_DIR})
set(CDK_ROOT "${LAWREC_SDK_ROOT}/src/common/cdk/user")
set(MAPI_ROOT "${CDK_ROOT}/mapi")
set(DATAFIFO_ROOT "${CDK_ROOT}/component/datafifo")
set(LIVE_ROOT "${CDK_ROOT}/thirdparty/live")
set(MPP_API_ROOT "${LAWREC_SDK_ROOT}/src/big/mpp/userapps/api")
set(MPP_LIB_ROOT "${LAWREC_SDK_ROOT}/src/big/mpp/userapps/lib")
set(IPCMSG_INC_DIR "${LAWREC_IPCMSG_INC_DIR}")
set(MPP_INC_DIR "${LAWREC_MPP_INC_DIR}")
set(MPP_COMM_INC_DIR "${LAWREC_MPP_COMM_INC_DIR}")
set(IPCMSG_LIB_DIR "${LAWREC_IPCMSG_LIB_DIR}")

if(IPCMSG_INC_DIR STREQUAL "")
    set(IPCMSG_INC_DIR "${IPCMSG_PATH}/include")
endif()

if(MPP_INC_DIR STREQUAL "")
    set(MPP_INC_DIR "${MPP_PATH}/include")
endif()

if(MPP_COMM_INC_DIR STREQUAL "")
    set(MPP_COMM_INC_DIR "${MPP_PATH}/include/comm")
endif()

if(IPCMSG_LIB_DIR STREQUAL "")
    set(IPCMSG_LIB_DIR "${IPCMSG_PATH}/host/lib")
endif()

target_include_directories(${EXE_TARGET}
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../common"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../control/include"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../record/include"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../rtsp/include"
    PRIVATE ${IPCMSG_INC_DIR}
    PRIVATE ${MPP_INC_DIR}
    PRIVATE ${MPP_COMM_INC_DIR}
    PRIVATE "${MAPI_ROOT}/include"
    PRIVATE "${MAPI_ROOT}/include/api"
    PRIVATE "${MAPI_ROOT}/include/comm"
    PRIVATE "${CDK_ROOT}/middleware/mp4_format/include"
    PRIVATE "${LIVE_ROOT}/BasicUsageEnvironment/include"
    PRIVATE "${LIVE_ROOT}/groupsock/include"
    PRIVATE "${LIVE_ROOT}/liveMedia/include"
    PRIVATE "${LIVE_ROOT}/UsageEnvironment/include"
    PRIVATE "${MPP_API_ROOT}")

target_link_directories(${EXE_TARGET}
    PRIVATE ${IPCMSG_LIB_DIR}
    PRIVATE "${MAPI_ROOT}/mediaclient/lib"
    PRIVATE "${CDK_ROOT}/middleware/mp4_format/lib"
    PRIVATE "${DATAFIFO_ROOT}/slave/lib"
    PRIVATE "${LIVE_ROOT}/BasicUsageEnvironment"
    PRIVATE "${LIVE_ROOT}/groupsock"
    PRIVATE "${LIVE_ROOT}/liveMedia"
    PRIVATE "${LIVE_ROOT}/UsageEnvironment"
    PRIVATE "${MPP_LIB_ROOT}")

target_link_libraries(${EXE_TARGET}
    PRIVATE "-Wl,--start-group"
    PRIVATE lvgl::lvgl
    PRIVATE lvgl::drivers
    PRIVATE pthread
    PRIVATE ipcmsg
    PRIVATE mapi
    PRIVATE mp4
    PRIVATE datafifo
    PRIVATE liveMedia
    PRIVATE UsageEnvironment
    PRIVATE BasicUsageEnvironment
    PRIVATE groupsock
    PRIVATE sys
    PRIVATE "-Wl,--end-group")

target_compile_definitions(${EXE_TARGET}
    PRIVATE DATA_FILE_PATH="${CMAKE_INSTALL_PREFIX}/${FOLDER_NAME}/data/"
    PRIVATE NO_OPENSSL=1)

install(TARGETS ${EXE_TARGET}
    RUNTIME DESTINATION ${FOLDER_NAME})

install(DIRECTORY
    "${CMAKE_CURRENT_LIST_DIR}/data"
    DESTINATION ${FOLDER_NAME})

if(DEMO_BUILD)
    set(DEMO_TARGET "${FOLDER_NAME}-demo")

    aux_source_directory(demo DEMO_SRC)

    add_executable(${DEMO_TARGET}
        ${DEMO_SRC})

    target_link_libraries(${DEMO_TARGET}
        PRIVATE lvgl::lvgl
        PRIVATE lvgl::drivers)

    install(TARGETS ${DEMO_TARGET}
        RUNTIME DESTINATION ${FOLDER_NAME})
endif()
