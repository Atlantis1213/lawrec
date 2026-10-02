cmake_minimum_required(VERSION 3.2)
set(K230_SDK_ROOT "$ENV{K230_SDK_ROOT}" CACHE PATH "K230 SDK dependency root")
if(NOT EXISTS "${K230_SDK_ROOT}/src/big/nncase/examples/cmake/Riscv64.cmake")
    message(FATAL_ERROR "Set K230_SDK_ROOT to the SDK root; use tools/build.sh")
endif()
include(${K230_SDK_ROOT}/src/big/nncase/examples/cmake/Riscv64.cmake)

project(lawrec C CXX)
set(nncase_sdk_root "${K230_SDK_ROOT}/src/big/nncase")
set(k230_sdk "${K230_SDK_ROOT}")
set(CMAKE_EXE_LINKER_FLAGS "-T ${nncase_sdk_root}/examples/cmake/link.lds --static -Wl,--allow-multiple-definition")
set(src
    main.cc
    model.cc
    mobile_retinaface.cc
    anchors_320.cc
    util.cc
    mobile_face.cc
    src/preview/lawrec_preview.cpp
    src/common/lawrec_ipc_server.cpp
    src/common/lawrec_display.cpp
    src/common/lawrec_rtsp_compat.cpp)
set(bin lawrec.elf)
set(script ./script/init.sh)
set(CMAKE_INSTALL_PREFIX "${PROJECT_SOURCE_DIR}/../" CACHE STRING "The path to use for make install" FORCE)
# The SDK toolchain appends these flags on every include. Remove the historical
# cache accumulation and set them once on this target, preserving other user flags.
set(lawrec_sdk_flags "(^|[ \t])(-fopenmp|-march=rv64imafdcv|-mabi=lp64d|-mcmodel=medany)([ \t]|$)")
foreach(language C CXX)
    set(flags "${CMAKE_${language}_FLAGS}")
    while("${flags}" MATCHES "${lawrec_sdk_flags}")
        string(REGEX REPLACE "${lawrec_sdk_flags}" " " flags "${flags}")
    endwhile()
    string(STRIP "${flags}" flags)
    set(CMAKE_${language}_FLAGS "${flags}" CACHE STRING "User ${language} flags" FORCE)
endforeach()
set(k230_opencv ${k230_sdk}/src/big/utils/lib/opencv)
set(CMAKE_INSTALL_PREFIX "../")
include_directories(${k230_opencv}/include/opencv4/)
link_directories(${k230_opencv}/lib  ${k230_opencv}/lib/opencv4/3rdparty)

include_directories(${nncase_sdk_root}/riscv64)
include_directories(${nncase_sdk_root}/riscv64/nncase/include)
include_directories(${nncase_sdk_root}/riscv64/nncase/include/nncase/runtime)
include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/src/common)
include_directories(${PROJECT_SOURCE_DIR}/src/preview)
include_directories(${nncase_sdk_root}/riscv64/rvvlib/include)
include_directories(${k230_sdk}/src/big/mpp/userapps/api/)
include_directories(${k230_sdk}/src/big/mpp/include)
include_directories(${k230_sdk}/src/big/mpp/include/comm)
include_directories(${k230_sdk}/src/big/mpp/userapps/sample/sample_vo)
include_directories(${k230_sdk}/src/common/cdk/user/component/ipcmsg/include)
include_directories(${k230_sdk}/src/common/cdk/user/component/ipcmsg/sample)
include_directories(${k230_sdk}/src/common/cdk/user/component/datafifo/include)
include_directories(${k230_sdk}/src/common/cdk/user/mapi/include)
include_directories(${k230_sdk}/src/common/cdk/user/mapi/include/api)
include_directories(${k230_sdk}/src/common/cdk/user/mapi/include/comm)
include_directories(${k230_sdk}/src/rt-smart/kernel/rt-thread/libcpu/risc-v/virt64)

link_directories(${k230_sdk}/src/big/mpp/userapps/lib)
link_directories(${nncase_sdk_root}/riscv64/rvvlib/)
link_directories(${nncase_sdk_root}/riscv64/nncase/lib/)
link_directories(${k230_sdk}/src/common/cdk/user/component/ipcmsg/host/lib)
link_directories(${k230_sdk}/src/common/cdk/user/component/datafifo/host/lib)
link_directories(${k230_sdk}/src/common/cdk/user/mapi/mediaserver/lib)
link_directories(${k230_sdk}/src/big/rt-smart/userapps/sdk/rt-thread/lib/risc-v/rv64)

add_executable(${bin} ${src})
target_compile_options(${bin} PRIVATE -fopenmp -march=rv64imafdcv -mabi=lp64d -mcmodel=medany)
# CMake 3.2 has no target_link_options; keep the compiler driver's ABI/OpenMP
# selection on the link step as well as compilation.
set_property(TARGET ${bin} APPEND_STRING PROPERTY LINK_FLAGS " -fopenmp -march=rv64imafdcv -mabi=lp64d -mcmodel=medany")
target_link_libraries(${bin}
    -Wl,--start-group
    rvv
    Nncase.Runtime.Native
    nncase.rt_modules.k230
    functional_k230
    sys
    vicap
    vb
    cam_device
    cam_engine
    hal
    oslayer
    ebase
    fpga
    isp_drv
    binder
    auto_ctrol
    common
    cam_caldb
    isi
    3a
    buffer_management
    cameric_drv
    video_in
    virtual_hal
    start_engine
    cmd_buffer
    switch
    cameric_reg_drv
    t_database_c
    t_mxml_c
    t_json_c
    t_common_c
    vo
    sensor
    connector
    atomic
    ipcmsg
    ai
    ao
    aenc
    adec
    venc
    vdec
    vdss
    dma
    vvi
    pm
    fft
    vg_lite
    vg_lite_util
    -Wl,--end-group
    opencv_core
    opencv_imgcodecs
    opencv_imgproc
    opencv_highgui
    opencv_videoio
    opencv_core
    zlib
    libjpeg-turbo
    libopenjp2
    libpng
    libtiff
    libwebp
    csi_cv
    mapi
    ipcmsg
    datafifo
    dpu
    rtthread)

install(TARGETS ${bin} DESTINATION out)
