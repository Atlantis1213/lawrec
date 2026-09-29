ifeq ($(wildcard /.dockerenv),)
$(error Build inside Docker using the project tools/build.sh)
endif
ifndef K230_SDK_ROOT
$(error K230_SDK_ROOT must identify the external SDK)
endif
CDK_SRC_DIR := $(K230_SDK_ROOT)/src/common/cdk
MPP_SRC_DIR := $(K230_SDK_ROOT)/src/big/mpp
TOOLCHAIN_BIN ?= /opt/toolchain/Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0/bin
CXX := $(TOOLCHAIN_BIN)/riscv64-unknown-linux-gnu-g++

IPCMSG_ROOT := $(CDK_SRC_DIR)/user/component/ipcmsg
DATAFIFO_ROOT := $(CDK_SRC_DIR)/user/component/datafifo
MAPI_ROOT := $(CDK_SRC_DIR)/user/mapi
LIVE_ROOT := $(CDK_SRC_DIR)/user/thirdparty/live
MPP_API_ROOT := $(MPP_SRC_DIR)/userapps/api
MPP_LIB_ROOT := $(MPP_SRC_DIR)/userapps/lib

OUT_DIR ?= $(abspath $(CURDIR)/../../../..)/out/service
BIN := $(OUT_DIR)/lawrec_service

INCS := -I$(CURDIR)/../common
INCS += -I$(CURDIR)/../control/include
INCS += -I$(CURDIR)/../record/include
INCS += -I$(CURDIR)/../rtsp/include
INCS += -I$(IPCMSG_ROOT)/include
INCS += -I$(MAPI_ROOT)/include
INCS += -I$(MAPI_ROOT)/include/api
INCS += -I$(MAPI_ROOT)/include/comm
INCS += -I$(CDK_SRC_DIR)/user/middleware/mp4_format/include
INCS += -I$(MPP_SRC_DIR)/include
INCS += -I$(MPP_SRC_DIR)/include/comm
INCS += -I$(MPP_API_ROOT)
INCS += -I$(LIVE_ROOT)/BasicUsageEnvironment/include
INCS += -I$(LIVE_ROOT)/groupsock/include
INCS += -I$(LIVE_ROOT)/liveMedia/include
INCS += -I$(LIVE_ROOT)/UsageEnvironment/include
INCS += -I/usr/local/include

LIBPATH := -L$(IPCMSG_ROOT)/host/lib
LIBPATH += -L$(DATAFIFO_ROOT)/slave/lib
LIBPATH += -L$(MAPI_ROOT)/mediaclient/lib
LIBPATH += -L$(CDK_SRC_DIR)/user/middleware/mp4_format/lib
LIBPATH += -L$(LIVE_ROOT)/BasicUsageEnvironment
LIBPATH += -L$(LIVE_ROOT)/groupsock
LIBPATH += -L$(LIVE_ROOT)/liveMedia
LIBPATH += -L$(LIVE_ROOT)/UsageEnvironment
LIBPATH += -L$(MPP_LIB_ROOT)
LIBPATH += -L/usr/local/lib

LIBS := -lipcmsg
LIBS += -lmapi
LIBS += -lmp4
LIBS += -ldatafifo
LIBS += -lliveMedia
LIBS += -lUsageEnvironment
LIBS += -lBasicUsageEnvironment
LIBS += -lgroupsock
LIBS += -lpthread

CXXFLAGS += -DNO_OPENSSL=1

SRCS := $(CURDIR)/src/main.cpp
SRCS += $(wildcard $(CURDIR)/../control/src/*.cpp)
SRCS += $(wildcard $(CURDIR)/../record/src/*.cpp)
SRCS += $(CURDIR)/../rtsp/main.cpp
SRCS += $(wildcard $(CURDIR)/../rtsp/src/*.cpp)
OBJS := $(OUT_DIR)/src/main.o $(OUT_DIR)/rtsp_main.o
OBJS += $(patsubst $(CURDIR)/../control/src/%.cpp,$(OUT_DIR)/control_src/%.o,$(wildcard $(CURDIR)/../control/src/*.cpp))
OBJS += $(patsubst $(CURDIR)/../record/src/%.cpp,$(OUT_DIR)/record_src/%.o,$(wildcard $(CURDIR)/../record/src/*.cpp))
OBJS += $(patsubst $(CURDIR)/../rtsp/src/%.cpp,$(OUT_DIR)/rtsp_src/%.o,$(wildcard $(CURDIR)/../rtsp/src/*.cpp))

.PHONY: all clean

all: $(BIN)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(OUT_DIR)/src:
	mkdir -p $(OUT_DIR)/src

$(OUT_DIR)/rtsp_src:
	mkdir -p $(OUT_DIR)/rtsp_src

$(OUT_DIR)/control_src:
	mkdir -p $(OUT_DIR)/control_src

$(OUT_DIR)/record_src:
	mkdir -p $(OUT_DIR)/record_src

$(OUT_DIR)/src/main.o: $(CURDIR)/src/main.cpp | $(OUT_DIR) $(OUT_DIR)/src
	$(CXX) -c $(CXXFLAGS) $(INCS) $< -o $@

$(OUT_DIR)/rtsp_main.o: $(CURDIR)/../rtsp/main.cpp | $(OUT_DIR)
	$(CXX) -c $(CXXFLAGS) $(INCS) $< -o $@

$(OUT_DIR)/control_src/%.o: $(CURDIR)/../control/src/%.cpp | $(OUT_DIR) $(OUT_DIR)/control_src
	$(CXX) -c $(CXXFLAGS) $(INCS) $< -o $@

$(OUT_DIR)/record_src/%.o: $(CURDIR)/../record/src/%.cpp | $(OUT_DIR) $(OUT_DIR)/record_src
	$(CXX) -c $(CXXFLAGS) $(INCS) $< -o $@

$(OUT_DIR)/rtsp_src/%.o: $(CURDIR)/../rtsp/src/%.cpp | $(OUT_DIR) $(OUT_DIR)/rtsp_src
	$(CXX) -c $(CXXFLAGS) $(INCS) $< -o $@

$(BIN): $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LIBPATH) -Wl,--start-group $(LIBS) -Wl,--end-group

clean:
	rm -rf $(OUT_DIR)
