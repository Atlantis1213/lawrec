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
add_executable(vision.elf main.cpp)
target_compile_options(vision.elf PRIVATE -Wall -Wextra -O2 -march=rv64imafdcv -mabi=lp64d -mcmodel=medany)
target_include_directories(vision.elf PRIVATE ../common
    "${MPP}/include" "${MPP}/include/comm" "${MPP}/userapps/api"
    "${CDK}/mapi/include" "${CDK}/mapi/include/api" "${CDK}/mapi/include/comm"
    "${CDK}/component/ipcmsg/include" "${CDK}/component/datafifo/include")
target_link_directories(vision.elf PRIVATE "${MPP}/userapps/lib"
    "${CDK}/component/ipcmsg/host/lib" "${CDK}/component/datafifo/host/lib"
    "${CDK}/mapi/mediaserver/lib"
    "${SDK}/src/big/rt-smart/userapps/sdk/rt-thread/lib/risc-v/rv64")
target_link_options(vision.elf PRIVATE -static "-T${NN}/examples/cmake/link.lds"
    -march=rv64imafdcv -mabi=lp64d -mcmodel=medany)
target_link_libraries(vision.elf PRIVATE -Wl,--start-group sys vicap vb vo sensor connector
    mapi ipcmsg datafifo venc aenc ai ao adec vdec vdss dma dpu rtthread -Wl,--end-group)
