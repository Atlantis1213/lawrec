cmake_minimum_required(VERSION 3.16)
set(SDK "$ENV{K230_SDK_ROOT}")
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER riscv64-unknown-linux-musl-gcc)
set(CMAKE_CXX_COMPILER riscv64-unknown-linux-musl-g++)
project(vision LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(NN "${SDK}/src/big/nncase")
set(MPP "${SDK}/src/big/mpp")
set(CDK "${SDK}/src/common/cdk/user")
add_executable(vision.elf main.cpp camera.cpp detector.cpp osd.cpp control.cpp ../common/faces.cpp)
target_compile_options(vision.elf PRIVATE -Wall -Wextra -O2 -march=rv64imafdcv -mabi=lp64d -mcmodel=medany)
target_include_directories(vision.elf PRIVATE ../common
    "${MPP}/include" "${MPP}/include/comm" "${MPP}/userapps/api"
    "${CDK}/mapi/include" "${CDK}/mapi/include/api" "${CDK}/mapi/include/comm"
    "${CDK}/component/ipcmsg/include" "${CDK}/component/datafifo/include"
    "${NN}/riscv64" "${NN}/riscv64/nncase/include" "${NN}/riscv64/nncase/include/nncase/runtime"
    "${NN}/riscv64/rvvlib/include")
target_link_directories(vision.elf PRIVATE "${MPP}/userapps/lib"
    "${CDK}/component/ipcmsg/host/lib" "${CDK}/component/datafifo/host/lib"
    "${CDK}/mapi/mediaserver/lib"
    "${SDK}/src/big/rt-smart/userapps/sdk/rt-thread/lib/risc-v/rv64"
    "${NN}/riscv64/nncase/lib" "${NN}/riscv64/rvvlib")
target_link_options(vision.elf PRIVATE -static "-T${NN}/examples/cmake/link.lds"
    -march=rv64imafdcv -mabi=lp64d -mcmodel=medany -fopenmp)
target_link_libraries(vision.elf PRIVATE -Wl,--start-group sys vicap vb vo sensor connector
    cam_device cam_engine hal oslayer ebase fpga isp_drv binder auto_ctrol common
    cam_caldb isi 3a buffer_management cameric_drv video_in virtual_hal start_engine
    cmd_buffer switch cameric_reg_drv t_database_c t_mxml_c t_json_c t_common_c
    mapi ipcmsg datafifo venc aenc ai ao adec vdec vdss dma dpu vvi rtthread atomic
    rvv Nncase.Runtime.Native nncase.rt_modules.k230 functional_k230
    -Wl,--end-group)
