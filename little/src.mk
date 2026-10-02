################################################################################
#
# lawrec
#
################################################################################
LAWREC_SITE = $(realpath $(TOPDIR))/../package/lawrec/src
LAWREC_SITE_METHOD = file

LAWREC_DEPENDENCIES += libdisp libdrm

LAWREC_SDK_TOP_CONFIG = $(realpath $(TOPDIR)/../../../..)/.config
LAWREC_SDK_ROOT = $(realpath $(TOPDIR)/../../../..)
LAWREC_IPCMSG_INC = $(LAWREC_SDK_ROOT)/src/common/cdk/user/component/ipcmsg/include
LAWREC_MPP_INC = $(LAWREC_SDK_ROOT)/src/big/mpp/include
LAWREC_MPP_COMM_INC = $(LAWREC_MPP_INC)/comm
LAWREC_BOARD_DEFS =

ifeq ($(shell test -f $(LAWREC_SDK_TOP_CONFIG) && grep -q '^CONFIG_BOARD_K230_CANMV_LCKFB=y' $(LAWREC_SDK_TOP_CONFIG) && echo y),y)
LAWREC_BOARD_DEFS += -DCONFIG_BOARD_K230_CANMV_LCKFB=1
LAWREC_CONF_OPTS += -DLAWREC_BOARD_LCKFB=ON
endif

LAWREC_CFLAGS = $(TARGET_CFLAGS) \
	$(LAWREC_BOARD_DEFS) \
	-I$(LAWREC_IPCMSG_INC) \
	-I$(LAWREC_MPP_INC) \
	-I$(LAWREC_MPP_COMM_INC) \
	-I$(STAGING_DIR)/usr/include \
	-I$(STAGING_DIR)/usr/include/libdrm

LAWREC_CXXFLAGS = $(LAWREC_CFLAGS)

LAWREC_CONF_OPTS += \
	-DLAWREC_IPCMSG_INC_DIR="$(LAWREC_IPCMSG_INC)" \
	-DLAWREC_MPP_INC_DIR="$(LAWREC_MPP_INC)" \
	-DLAWREC_MPP_COMM_INC_DIR="$(LAWREC_MPP_COMM_INC)" \
	-DLAWREC_IPCMSG_LIB_DIR="$(LAWREC_SDK_ROOT)/src/common/cdk/user/component/ipcmsg/host/lib" \
	-DLAWREC_SDK_ROOT="$(LAWREC_SDK_ROOT)" \
	-DLAWREC_BUILD_SERVICE=OFF \
	-DCMAKE_CXX_FLAGS="$(LAWREC_CXXFLAGS)" \
	-DCMAKE_C_FLAGS="$(LAWREC_CFLAGS)" \
	-DNATIVE_BUILD=OFF

LAWREC_EXTRA_DOWNLOADS = https://github.com/lvgl/lvgl/archive/refs/tags/v8.3.1.tar.gz

define lawrec_rsync
	rsync -a --chmod=u=rwX,go=rX --exclude out --exclude build --exclude .svn --exclude .git --exclude .hg --exclude .bzr --exclude CVS ${LAWREC_SITE}/ $(@D)
endef

define LAWREC_EXTRACT_CMDS
	$(call lawrec_rsync)
	mkdir -p $(@D)/thirdlib/lvgl
	tar -xf $(LAWREC_DL_DIR)/v8.3.1.tar.gz -C $(@D)/thirdlib/lvgl --strip-components=1
endef

LAWREC_PRE_BUILD_HOOKS += lawrec_pre_build_hook
LAWREC_PRE_CONFIGURE_HOOKS += lawrec_pre_build_hook
define lawrec_pre_build_hook
	$(call lawrec_rsync)
endef

LAWREC_POST_INSTALL_TARGET_HOOKS += lawrec_install_init_script
define lawrec_install_init_script
	$(INSTALL) -D -m 0755 $(TOPDIR)/../package/lawrec/S99lawrec $(TARGET_DIR)/etc/init.d/S99lawrec
	$(INSTALL) -D -m 0755 $(TOPDIR)/../package/lawrec/S45wifi $(TARGET_DIR)/etc/init.d/S45wifi
	rm -f $(TARGET_DIR)/app/lawrec/rtsp_start.sh
	rm -f $(TARGET_DIR)/app/lawrec/rtsp_stop.sh
	rm -f $(TARGET_DIR)/app/lawrec/rtsp_status.sh
	rm -f $(TARGET_DIR)/app/lawrec/service/lawrec_service
	rm -f $(TARGET_DIR)/app/lawrec/xiaodemo/xiaodemo
endef

$(eval $(cmake-package))
