#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <sys/prctl.h>
#include <nncase/runtime/runtime_op_utility.h>
#include "mobile_retinaface.h"
#include "mobile_face.h"
#include "lawrec_rtsp_compat.h"
#include "lawrec_preview.h"
#include "../little/src/common/lawrec_preview_wire.h"
#include "../little/src/common/lawrec_playback_wire.h"
#include "util.h"
#include "mpi_sys_api.h"

#include "k_connector_comm.h"
#include "mpi_connector_api.h"

using namespace nncase;
using namespace nncase::runtime;
using namespace nncase::runtime::detail;


/* vicap */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <signal.h>
#include <atomic>
#include <fcntl.h>
#include "k_module.h"
#include <dirent.h>
#include "k_type.h"
#include "k_vb_comm.h"
#include "k_video_comm.h"
#include "k_sys_comm.h"
#include "mpi_vb_api.h"
#include "mpi_vicap_api.h"
#include "mpi_isp_api.h"
#include "mpi_sys_api.h"
#include "k_vo_comm.h"
#include "mpi_vo_api.h"
#include "sys/ioctl.h"
#include "vo_test_case.h"
#include "mpi_dma_api.h"
#include "k_ipcmsg.h"
#include "sample_define.h"
#include "k_autoconf_comm.h"
#include "lawrec_runtime_config.h"
#include "vi_vo.h"
#include "mapi_sys_api.h"

#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
#define LAWREC_FACE_DATA_BASE 0x3FB00000
#define LAWREC_FACE_DATA_SIZE 0x00040000
#else
#ifndef CONFIG_MEM_FACE_DATA_BASE
#define CONFIG_MEM_FACE_DATA_BASE 0x07c00000
#endif

#ifndef CONFIG_MEM_FACE_DATA_SIZE
#define CONFIG_MEM_FACE_DATA_SIZE 0x00040000
#endif
#define LAWREC_FACE_DATA_BASE CONFIG_MEM_FACE_DATA_BASE
#define LAWREC_FACE_DATA_SIZE CONFIG_MEM_FACE_DATA_SIZE
#endif

#define FEATURE_SIZE 768

#define CHANNEL 3
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
#define ISP_CHN1_HEIGHT (720)
#define ISP_CHN1_WIDTH  (1280)
#define ISP_INPUT_WIDTH (1920)
#define ISP_INPUT_HEIGHT (1080)
#define DISP_ROTATION   K_ROTATION_90
#define LAWREC_ENABLE_FACE_DB 0
#else
#define ISP_CHN1_HEIGHT (1280)
#define ISP_CHN1_WIDTH  (720)

#define ISP_INPUT_WIDTH (2592)
#define ISP_INPUT_HEIGHT (1944)
#define ISP_CROP_W_OFFSET (768)
#define ISP_CROP_H_OFFSET (16)
#define DISP_ROTATION   K_ROTATION_0
#define LAWREC_ENABLE_FACE_DB 1
#endif
#define LED_PIN_NUM1    33
#define LED_PIN_NUM2    32
#define TEST_BOOT_TIME

#ifdef TEST_BOOT_TIME
#define TIME_RATE (1600*1000)
#define SEND_TIME_INTERVAL_US   (100 * 1000)
uint64_t perf_get_smodecycles(void);
typedef struct kd_pin_mode
{
    unsigned short pin;     /* pin number, from 0 to 63 */
    unsigned short mode;    /* pin level status, 0 low level, 1 high level */
} pin_mode_t;

#define KD_GPIO_HIGH     1
#define KD_GPIO_LOW      0

#define	GPIO_DM_OUTPUT           _IOW('G', 0, int)
#define	GPIO_DM_INPUT            _IOW('G', 1, int)
#define	GPIO_DM_INPUT_PULL_UP    _IOW('G', 2, int)
#define	GPIO_DM_INPUT_PULL_DOWN  _IOW('G', 3, int)
#define	GPIO_WRITE_LOW           _IOW('G', 4, int)
#define	GPIO_WRITE_HIGH          _IOW('G', 5, int)

#define	GPIO_PE_RISING           _IOW('G', 7, int)
#define	GPIO_PE_FALLING          _IOW('G', 8, int)
#define	GPIO_PE_BOTH             _IOW('G', 9, int)
#define	GPIO_PE_HIGH             _IOW('G', 10, int)
#define	GPIO_PE_LOW              _IOW('G', 11, int)

#define GPIO_READ_VALUE       	_IOW('G', 12, int)

feature_db_t *mem_feature_data;
static bool key_press = false;
bool sensor_process = true;
bool is_clear_feature = false;
std::string name;
k_s32 s32Id1;
pthread_t threadid1;
pthread_t ipc_run_handle;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
char dir_name[40];
int mem_fd = -1;
int gpio_write_high(int fd)
{
    int ret;
    pin_mode_t mode27;
    mode27.pin = LED_PIN_NUM2;
    ioctl(fd, GPIO_WRITE_HIGH, &mode27);
    return ret;
}

int gpio_write_low(int fd)
{
    int ret;
    pin_mode_t mode27;
    mode27.pin = LED_PIN_NUM2;
    ioctl(fd, GPIO_WRITE_LOW, &mode27);
    return ret;
}

int sample_gpio_op(void)
{
    int ret;
    int gpio_fd = -1;
    pin_mode_t mode27;
    mode27.pin = LED_PIN_NUM2;
    gpio_fd = open("/dev/gpio", O_RDWR);
    if (gpio_fd < 0)
    {
        perror("open /dev/pin err\n");
        return -1;
    }
    ret = ioctl(gpio_fd, GPIO_DM_OUTPUT, &mode27);
    return gpio_fd;
}




static int test_fd;
static k_u64 start_time,stop_time;
#define TEST_TIME(func, func_name)     start_time = perf_get_smodecycles(); \
                                       func; \
                                       stop_time = perf_get_smodecycles(); \
                                       printf("%s use time:%d ms\n", func_name,(stop_time - start_time) / TIME_RATE);

static inline void TEST_BOOT_TIME_INIT(void)
{
    test_fd = sample_gpio_op();
    // gpio_write_high(test_fd);
    // gpio_write_low(test_fd);
}


static inline void TEST_BOOT_TIME_TRIGER(void)
{
    gpio_write_high(test_fd);
    gpio_write_low(test_fd);
}

static inline void PRINT_TIME_NOW(void)
{
    printf("current time:%ld ms\n", perf_get_smodecycles()/TIME_RATE);
}
#else
#define TEST_TIME(func, func_name)  func
#define TEST_BOOT_TIME_INIT()
#define TEST_BOOT_TIME_TRIGER()
#define PRINT_TIME_NOW()
#endif

int get_keyvalue(int fd)
{
    int ret;
    pin_mode_t mode32;
    mode32.pin = LED_PIN_NUM2;
    ioctl(fd, GPIO_READ_VALUE, &mode32);
    return mode32.mode;
}
extern k_s32 kd_display_set_backlight(void);
extern k_s32 kd_display_reset(void);
int sample_sys_bind_init(void);
static void sample_vicap_unbind_vo(k_mpp_chn vicap_mpp_chn, k_mpp_chn vo_mpp_chn);

std::atomic<bool> quit(true);
static LawrecPreviewController g_preview;
static bool g_playback_display = false;

/* Only switch video layers; connector/DSI/VO and Linux UI stay alive. */
static int playback_display(const lawrec_playback_wire_t &request)
{
    if (!g_preview.BackendReady()) return -11;
    if (!request.enabled) {
        int ret = kd_mpi_vo_disable_video_layer(K_VO_LAYER0);
        if (ret) return ret;
        ret = kd_mpi_vo_enable_video_layer(K_VO_LAYER1);
        if (!ret) g_playback_display = false;
        return ret;
    }
    if (g_preview.Enabled() || g_preview.Bound()) return -16;
    if (request.width != 1280 || request.height != 720) return -22;
    k_vo_video_layer_attr attr = {};
    attr.img_size.width = request.width;
    attr.img_size.height = request.height;
    attr.pixel_format = PIXEL_FORMAT_YVU_PLANAR_420;
    attr.stride = (request.width / 8 - 1) | ((request.height - 1) << 16);
    attr.func = K_VO_SCALER_ENABLE;
    attr.scaler_attr.out_size.width = 480;
    attr.scaler_attr.out_size.height = 270;
    attr.scaler_attr.stride = (480 / 8 - 1) | ((270 - 1) << 16);
    attr.display_rect.y = 200;
    int ret = kd_mpi_vo_set_video_layer_attr(K_VO_LAYER0, &attr);
    if (ret) return ret;
    ret = kd_mpi_vo_disable_video_layer(K_VO_LAYER1);
    if (ret) return ret;
    ret = kd_mpi_vo_enable_video_layer(K_VO_LAYER0);
    if (ret) kd_mpi_vo_enable_video_layer(K_VO_LAYER1);
    else g_playback_display = true;
    return ret;
}
static LawrecRtspCompat g_rtsp_compat(LAWREC_RTSP_DEFAULT_PORT,
                                      LAWREC_RTSP_DEFAULT_STREAM_NAME);

#define BLOCK_TIME              100

int key_values = 0;
int pressed_key = 0;
k_dma_dev_attr_t dma_dev_attr;
k_dma_chn_attr_u dma_chn_attr[DMA_MAX_CHN_NUMS];
k_video_frame_info df_info_dst;
bool app_run = true;

/*
 * Preview/display state currently lives in main.cc. Before larger module
 * extraction, keep the helpers grouped here so new features stop expanding the
 * main loop directly.
 */

static int lawrec_preview_enter(void)
{
    if (g_playback_display) return -16;
    return g_preview.Enter(sample_sys_bind_init);
}

static int lawrec_preview_exit(void)
{
    return g_preview.Exit(sample_vicap_unbind_vo);
}

static void lawrec_preview_force_unbind(void)
{
    g_preview.ForceUnbindAtStartup(sample_vicap_unbind_vo);
}

static int lawrec_preview_blank_init(void)
{
    return g_preview.InitBlankFrame();
}

static void lawrec_preview_blank_deinit(void)
{
    g_preview.DeinitBlankFrame();
}

static inline bool lawrec_ai_pipeline_enabled(void)
{
    return !LAWREC_STAGE0_PREVIEW_ONLY && LAWREC_ENABLE_AI_PIPELINE;
}

static inline bool lawrec_face_db_enabled(void)
{
    return lawrec_ai_pipeline_enabled() && LAWREC_ENABLE_FACE_DB;
}

static bool lawrec_rtsp_requested(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--rtsp") == 0)
            return true;
    }
    return false;
}

static void lawrec_rtsp_fill_status(lawrec_rtsp_status_t *status)
{
    g_rtsp_compat.FillStatus(status);
}

static int ipc_send_payload(ipc_msg_cmd_t cmd, const void *content, uint32_t content_size)
{
    k_ipcmsg_message_t *pReq;

    pReq = kd_ipcmsg_create_message(SEND_ONLY_MODULE_ID, cmd,
                                    const_cast<void *>(content), content_size);
    if (!pReq) return -1;
    int ret = kd_ipcmsg_send_only(s32Id1, pReq);
    kd_ipcmsg_destroy_message(pReq);
    if (ret) printf("[lawrec] ipc send cmd=%u error=%d\n", cmd, ret);
    return ret;
}

static int lawrec_rtsp_start(void)
{
    lawrec_rtsp_status_t status;
    int ret;

    ret = g_rtsp_compat.StartRequested();
    if (ret == 0) {
        lawrec_rtsp_fill_status(&status);
        ipc_send_payload(MSG_CMD_RTSP_STATUS, &status, sizeof(status));
    }
    return ret;
}

static int lawrec_rtsp_stop(void)
{
    lawrec_rtsp_status_t status;
    int ret = 0;

    ret = g_rtsp_compat.StopRequested();
    lawrec_rtsp_fill_status(&status);
    ipc_send_payload(MSG_CMD_RTSP_STATUS, &status, sizeof(status));
    return ret;
}

static void lawrec_clear_osd_frame(void *pic_vaddr, k_video_frame_info *vf_info)
{
    int ret;

    if (vicap_install_osd != 1 || pic_vaddr == NULL || vf_info == NULL)
        return;

    memset(pic_vaddr, 0, osd_width * osd_height * 4);
    ret = kd_mpi_vo_chn_insert_frame(osd_id + 3, vf_info);
    if (ret != 0)
        printf("[lawrec] clear osd failed ret=%d\n", ret);
}


void read_mem_feature()
{
    int feature_num = 0;
    mem_fd = open("/dev/mem", O_RDWR|O_SYNC);
    if (mem_fd < 0) {
        printf("open /dev/mem error\n");
        return;
    }
    mem_feature_data = (feature_db_t *)mmap(NULL, LAWREC_FACE_DATA_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, LAWREC_FACE_DATA_BASE);
    if(mem_feature_data->count > 100 )
    {
        mem_feature_data->count = 0;
    }
    l2normalize_feature_db(mem_feature_data,mem_feature_data->count);
    return;
}

int clear_feature(void)
{
    pthread_mutex_lock(&mutex);
    mem_feature_data->count = 0;
    pthread_mutex_unlock(&mutex);
    return 0;
}



int ipc_send_thread(ipc_msg_cmd_t cmd)
{
    void *content;
    uint8_t success = 0;
    uint8_t fail = 1;
    uint32_t size = 0;
    uint32_t cmd_feature[2];
    uint32_t content_size = 1;
    switch(cmd)
    {
        case MSG_CMD_SIGNUP_RESULT:
        case MSG_CMD_IMPORT_RESULT:
        case MSG_CMD_DELETE_RESULT:
        case MSG_CMD_PREVIEW_ENTER_RESULT:
        case MSG_CMD_PREVIEW_EXIT_RESULT:
        case MSG_CMD_PING_RESULT:
        case MSG_CMD_RTSP_START_RESULT:
        case MSG_CMD_RTSP_STOP_RESULT:
            content = &success;
            break;
        case MSG_CMD_FEATURE_SAVE:
            // sprintf((char*)content,"%x%d",CONFIG_MEM_FACE_DATA_BASE,size);
            if (mem_feature_data == nullptr) {
                printf("[lawrec] feature save skipped: feature db not ready\n");
                return -1;
            }
            size = mem_feature_data->count*sizeof(feature) + sizeof(uint32_t);
            cmd_feature[0] = LAWREC_FACE_DATA_BASE;
            cmd_feature[1] = size;
            content = cmd_feature;
            content_size = sizeof(cmd_feature);
            break;
        case MSG_CMD_ERROR:
            content = &fail;
            break;
        default:
            printf("can not recongnise ipc_msg cmd\n");
            return -1;
    }
    k_ipcmsg_message_t* pReq = kd_ipcmsg_create_message(SEND_ONLY_MODULE_ID, cmd, content, content_size);
    kd_ipcmsg_send_only(s32Id1, pReq);
    kd_ipcmsg_destroy_message(pReq);
    usleep(SEND_TIME_INTERVAL_US * 3);
    return 0;
}

/*
 * Historical doorlock business commands are still accepted here for
 * compatibility, but preview control is the only current lawrec primary path.
 */

void handle_feature(k_s32 s32Id, k_ipcmsg_message_t* msg)
{
    if (!msg || (msg->u32BodyLen && !msg->pBody)) return;
    if (msg->u32CMD == MSG_CMD_PLAYBACK_DISPLAY) {
        int result = -22;
        lawrec_playback_wire_t request = {};
        if (msg->pBody && msg->u32BodyLen == sizeof(request)) {
            memcpy(&request, msg->pBody, sizeof(request));
            if (request.version == LAWREC_PLAYBACK_VERSION && request.enabled <= 1)
                result = playback_display(request);
        }
        printf("[playback] display enabled=%u result=%d\n", request.enabled, result);
        auto *response = kd_ipcmsg_create_resp_message(msg, result, NULL, 0);
        if (response) {
            int ret = kd_ipcmsg_send_async(s32Id, response, NULL);
            if (ret) printf("[playback] response failed ret=%d\n", ret);
            kd_ipcmsg_destroy_message(response);
        }
        return;
    }
    if (msg->u32CMD == MSG_CMD_RTSP_START || msg->u32CMD == MSG_CMD_RTSP_STOP ||
        msg->u32CMD == MSG_CMD_RTSP_QUERY) {
        printf("[lawrec] unsupported RTSP business cmd=%u; Linux owns network output\n", msg->u32CMD);
        int8_t error = -1;
        ipc_send_payload(MSG_CMD_ERROR, &error, 1);
        return;
    }
    if (msg->u32CMD == MSG_CMD_IMPORT || msg->u32CMD == MSG_CMD_SIGNUP) {
        if (!msg->pBody || !msg->u32BodyLen || msg->u32BodyLen > sizeof(dir_name) ||
            !memchr(msg->pBody, 0, msg->u32BodyLen)) return;
    }
    if (msg->u32CMD == MSG_CMD_PREVIEW_ENTER || msg->u32CMD == MSG_CMD_PREVIEW_EXIT) {
        if (!msg->pBody || msg->u32BodyLen != sizeof(lawrec_preview_wire_t)) {
            printf("[lawrec] reject preview protocol length; update both cores\n");
            return;
        }
        lawrec_preview_wire_t wire;
        memcpy(&wire, msg->pBody, sizeof(wire));
        if (wire.version != LAWREC_PREVIEW_WIRE_VERSION) return;
        wire.result = msg->u32CMD == MSG_CMD_PREVIEW_ENTER ? lawrec_preview_enter() : lawrec_preview_exit();
        printf("[lawrec] preview cmd=%u seq=%u result=%d\n", msg->u32CMD, wire.sequence, wire.result);
        ipc_send_payload(msg->u32CMD == MSG_CMD_PREVIEW_ENTER ? MSG_CMD_PREVIEW_ENTER_RESULT : MSG_CMD_PREVIEW_EXIT_RESULT,
                         &wire, sizeof(wire));
        return;
    }
    k_s32 s32Ret = 0;
    char content[64];

    memset(content, 0, 64);
    switch(msg->u32Module)
    {
        case SEND_SYNC_MODULE_ID:
            snprintf(content, 64, "modle:%d, cmd:%08x, have done.", msg->u32Module, msg->u32CMD);
            s32Ret = 0;
            break;
        case SNED_ASYNC_MODULE_ID:
            snprintf(content, 64, "modle:%d, cmd:%08x, have done.", msg->u32Module, msg->u32CMD);
            s32Ret = 0;
            break;
        case SEND_ONLY_MODULE_ID:
            /*
            If a reply message is created for kd_ipcmsg_send_only,
            it will trigger the "Sync msg is too late" alert on the other side..
            */
            printf("receive msg from %d: %s, len: %d\n", s32Id, (char*)msg->pBody, msg->u32BodyLen);
            return;
        default:
            snprintf(content, 64, "modle:%d, cmd:%08x, is not found.", msg->u32Module, msg->u32CMD);
            s32Ret = -1;
    }
    switch(msg->u32CMD)
    {
        case MSG_CMD_IMPORT:
            printf("[lawrec] ipc import path=%s\n", (char *)msg->pBody);
            sensor_process = false;
            sprintf(dir_name,"%s",msg->pBody);
            ipc_send_thread(MSG_CMD_IMPORT_RESULT);
            break;
        case MSG_CMD_SIGNUP:
            printf("[lawrec] ipc signup name=%s\n", (char *)msg->pBody);
            key_press = true;
            name = (char*)msg->pBody;
            ipc_send_thread(MSG_CMD_SIGNUP_RESULT);
            break;
        case MSG_CMD_DELETE:
            printf("[lawrec] ipc delete\n");
            clear_feature();
            ipc_send_thread(MSG_CMD_DELETE_RESULT);
            ipc_send_thread(MSG_CMD_FEATURE_SAVE);
            break;
        case MSG_CMD_PING:
            printf("[lawrec] ipc ping\n");
            ipc_send_thread(MSG_CMD_PING_RESULT);
            break;
        case MSG_CMD_RTSP_START:
            printf("[lawrec] ipc rtsp start\n");
            if (lawrec_rtsp_start() == 0)
                ipc_send_thread(MSG_CMD_RTSP_START_RESULT);
            else
                ipc_send_thread(MSG_CMD_ERROR);
            break;
        case MSG_CMD_RTSP_STOP:
            printf("[lawrec] ipc rtsp stop\n");
            if (lawrec_rtsp_stop() == 0)
                ipc_send_thread(MSG_CMD_RTSP_STOP_RESULT);
            else
                ipc_send_thread(MSG_CMD_ERROR);
            break;
        case MSG_CMD_RTSP_QUERY: {
            lawrec_rtsp_status_t status;
            printf("[lawrec] ipc rtsp query\n");
            lawrec_rtsp_fill_status(&status);
            ipc_send_payload(MSG_CMD_RTSP_STATUS, &status, sizeof(status));
            break;
        }
        case MSG_CMD_PREVIEW_ENTER: {
            printf("[lawrec] ipc preview enter\n");
            int8_t result = lawrec_preview_enter() == 0 ? 0 : -1;
            ipc_send_payload(MSG_CMD_PREVIEW_ENTER_RESULT, &result, sizeof(result));
            break;
        }
        case MSG_CMD_PREVIEW_EXIT: {
            printf("[lawrec] ipc preview exit\n");
            int8_t result = lawrec_preview_exit() == 0 ? 0 : -1;
            ipc_send_payload(MSG_CMD_PREVIEW_EXIT_RESULT, &result, sizeof(result));
            break;
        }
        default:
            printf("can not recongnise ipc_msg cmd\n");
            break;
    }
}


static void* thread_ipcmsg(void* arg)
{
    kd_ipcmsg_run(s32Id1);
    return NULL;
}

void *ipc_msg_server(void *arg)
{

    int ret = 0;
    k_ipcmsg_connect_t stConnectAttr;

    stConnectAttr.u32RemoteId = 0;
    stConnectAttr.u32Port = 101;
    stConnectAttr.u32Priority = 0;
    kd_ipcmsg_add_service(LAWREC_IPC_SERVICE_NAME,&stConnectAttr);

    if(ret != 0)
    {
        printf("kd_ipcmsg_add_service return err:%x\n", ret);
    }
    ret = kd_ipcmsg_connect(&s32Id1, LAWREC_IPC_SERVICE_NAME, handle_feature);
    if(ret != 0)
    {
        printf("Connect fail\n");
    }
    kd_ipcmsg_run(s32Id1);
    return NULL;
}

static int lawrec_start_ipc_server(pthread_t *ipc_message_handle)
{
    if (LAWREC_STAGE0_PREVIEW_ONLY || ipc_message_handle == NULL)
        return 0;

    return pthread_create(ipc_message_handle, NULL, ipc_msg_server, NULL);
}

void fun_sig(int sig)
{
    if(sig == SIGINT)
    {
        printf("recive ctrl+c\n");
        app_run = false;
        quit.store(false);
    }
}


uint64_t perf_get_smodecycles(void)
{
    uint64_t cnt;
    __asm__ __volatile__(
        "rdcycle %0" : "=r"(cnt)
    );
    return cnt;
}

k_vo_draw_frame vo_frame = (k_vo_draw_frame) {
    1,
    16,
    16,
    128,
    128,
    1
};

static inline void map_detect_box_to_display(const face_coordinate &box, k_vo_draw_frame *frame)
{
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    frame->line_x_start = ISP_CHN0_HEIGHT - ((uint32_t)box.y2 * ISP_CHN0_HEIGHT / ISP_CHN1_HEIGHT);
    frame->line_y_start = ((uint32_t)box.x1) * ISP_CHN0_WIDTH / ISP_CHN1_WIDTH;
    frame->line_x_end = ISP_CHN0_HEIGHT - ((uint32_t)box.y1 * ISP_CHN0_HEIGHT / ISP_CHN1_HEIGHT);
    frame->line_y_end = ((uint32_t)box.x2) * ISP_CHN0_WIDTH / ISP_CHN1_WIDTH;
#else
    frame->line_x_start = ((uint32_t)box.x1) * ISP_CHN0_WIDTH / ISP_CHN1_WIDTH;
    frame->line_y_start = ((uint32_t)box.y1) * ISP_CHN0_HEIGHT / ISP_CHN1_HEIGHT;
    frame->line_x_end = ((uint32_t)box.x2) * ISP_CHN0_WIDTH / ISP_CHN1_WIDTH;
    frame->line_y_end = ((uint32_t)box.y2) * ISP_CHN0_HEIGHT / ISP_CHN1_HEIGHT;
#endif
}

static inline cv::Point2f map_landmark_to_display(float x, float y)
{
    cv::Point2f point;
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    point.x = ISP_CHN0_HEIGHT - (y * ISP_CHN0_HEIGHT);
    point.y = x * ISP_CHN0_WIDTH;
#else
    point.x = x * 1920 - 420;
    point.y = y * 1920;
#endif
    return point;
}

static inline box_t map_detect_box_to_osd(const face_coordinate &box)
{
    box_t face_box;
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    face_box.x = (float)box.x1 * (float)osd_height / (float)ISP_CHN1_WIDTH;
    face_box.y = (float)box.y1 * (float)osd_width / (float)ISP_CHN1_HEIGHT;
    face_box.w = (float)(box.x2 - box.x1) * (float)osd_height / (float)ISP_CHN1_WIDTH;
    face_box.h = (float)(box.y2 - box.y1) * (float)osd_width / (float)ISP_CHN1_HEIGHT;
#else
    face_box.x = (float)box.x1 * (float)osd_width / (float)ISP_CHN1_WIDTH;
    face_box.y = (float)box.y1 * (float)osd_height / (float)ISP_CHN1_HEIGHT;
    face_box.w = (float)(box.x2 - box.x1) * (float)osd_width / (float)ISP_CHN1_WIDTH;
    face_box.h = (float)(box.y2 - box.y1) * (float)osd_height / (float)ISP_CHN1_HEIGHT;
#endif
    if (face_box.w > 0.0f && face_box.w < 2.0f)
        face_box.w = 2.0f;
    if (face_box.h > 0.0f && face_box.h < 2.0f)
        face_box.h = 2.0f;
    return face_box;
}

static inline cv::Point2f map_landmark_to_osd(float x, float y)
{
    cv::Point2f point;
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    point.x = x * osd_height;
    point.y = y * osd_width;
#else
    point.x = x * osd_width;
    point.y = y * osd_height;
#endif
    return point;
}

static inline bool is_valid_detect_box(const face_coordinate &box)
{
    int x1 = box.x1 < 0 ? 0 : box.x1;
    int y1 = box.y1 < 0 ? 0 : box.y1;
    int x2 = box.x2 > ISP_CHN1_WIDTH ? ISP_CHN1_WIDTH : box.x2;
    int y2 = box.y2 > ISP_CHN1_HEIGHT ? ISP_CHN1_HEIGHT : box.y2;
    int w = x2 - x1;
    int h = y2 - y1;

    if (w <= 0 || h <= 0)
        return false;

    if (w > ISP_CHN1_WIDTH || h > ISP_CHN1_HEIGHT)
        return false;

    return true;
}

int vo_creat_layer_test(k_vo_layer chn_id, layer_info *info)
{
    k_vo_video_layer_attr attr;

    // check layer
    if ((chn_id >= K_MAX_VO_LAYER_NUM) || ((info->func & K_VO_SCALER_ENABLE) && (chn_id != K_VO_LAYER0))
            || ((info->func != 0) && (chn_id == K_VO_LAYER2)))
    {
        printf("input layer num failed \n");
        return -1 ;
    }

    // check scaler

    // set offset
    attr.display_rect = info->offset;
    // set act
    attr.img_size = info->act_size;
    // sget size
    info->size = info->act_size.height * info->act_size.width * 3 / 2;
    //set pixel format
    attr.pixel_format = info->format;
    if (info->format != PIXEL_FORMAT_YVU_PLANAR_420)
    {
        printf("input pix format failed \n");
        return -1;
    }
    // set stride
    attr.stride = (info->act_size.width / 8 - 1) + ((info->act_size.height - 1) << 16);
    // set function
    attr.func = info->func;
    // set scaler attr
    attr.scaler_attr = info->attr;

    // set video layer atrr
    kd_mpi_vo_set_video_layer_attr(chn_id, &attr);

    // enable layer
    kd_mpi_vo_enable_video_layer(chn_id);

    return 0;
}





k_s32 sample_connector_init(void)
{
    k_u32 ret = 0;
    k_s32 connector_fd;
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    k_connector_type connector_type = ST7701_V1_MIPI_2LAN_480X800_30FPS;
#else
    k_connector_type connector_type = HX8377_V2_MIPI_4LAN_1080X1920_30FPS;
#endif
    k_connector_info connector_info;

    memset(&connector_info, 0, sizeof(k_connector_info));

    //connector get sensor info
    ret = kd_mpi_get_connector_info(connector_type, &connector_info);
    if (ret) {
        printf("sample_vicap, the sensor type not supported!\n");
        return ret;
    }

    connector_fd = kd_mpi_connector_open(connector_info.connector_name);
    if (connector_fd < 0) {
        printf("%s, connector open failed.\n", __func__);
        return K_ERR_VO_NOTREADY;
    }

    // set connect power
    kd_mpi_connector_power_set(connector_fd, K_TRUE);
    // connector init
    kd_mpi_connector_init(connector_fd, connector_info);

    return 0;
}


static k_s32 vo_layer_vdss_bind_vo_config(void)
{
    layer_info info;
    k_vo_layer chn_id = K_VO_LAYER1;

    memset(&info, 0, sizeof(info));

    sample_connector_init();

#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    info.act_size.width = ISP_CHN0_HEIGHT;
    info.act_size.height = ISP_CHN0_WIDTH;
    info.format = PIXEL_FORMAT_YVU_PLANAR_420;
    info.func = DISP_ROTATION;
#else
    info.act_size.width = ISP_CHN0_WIDTH;//1080;//640;//1080;
    info.act_size.height = ISP_CHN0_HEIGHT;//1920;//480;//1920;
    info.format = PIXEL_FORMAT_YVU_PLANAR_420;
    info.func = DISP_ROTATION;
#endif
    info.global_alptha = 0xff;
    info.offset.x = 0;//(1080-w)/2,
    info.offset.y = 0;//(1920-h)/2;
    vo_creat_layer_test(chn_id, &info);
    if (vicap_install_osd == 1)
    {
        osd_info osd;
        osd.act_size.width = osd_width;
        osd.act_size.height = osd_height;
        osd.offset.x = 0;
        osd.offset.y = 0;
        osd.global_alptha = 0xff;
        // osd.global_alptha = 0x32;
        osd.format = PIXEL_FORMAT_ARGB_8888; // PIXEL_FORMAT_ARGB_4444; //PIXEL_FORMAT_ARGB_1555;//PIXEL_FORMAT_ARGB_8888;

        vo_creat_osd_test(osd_id, &osd);
    }
    else
    {
        printf("[lawrec-big] A/B test: K_VO_OSD3 disabled, keep connector/layer1/vo_enable/sys_bind only\n");
    }
    kd_mpi_vo_enable();
    return 0;
}

static void sample_vo_fn(void *arg)
{
    // set hardware reset;
    usleep(10000);
    vo_layer_vdss_bind_vo_config();
    return;
}

static int sample_vo_init(void)
{
    usleep(10000);
    vo_layer_vdss_bind_vo_config();
    return 0;
}

static void *sample_vo_thread(void *arg)
{
    TEST_TIME(sample_vo_fn(arg), "sample_vo_fn");
    return NULL;
}

k_vicap_dev vicap_dev;
k_vicap_chn vicap_chn;
k_vicap_dev_attr dev_attr;
k_vicap_chn_attr chn_attr;
k_vicap_sensor_info sensor_info;
k_vicap_sensor_type sensor_type;
k_video_frame_info dump_info;
k_vb_config config;

static void sample_vicap_unbind_vo(k_mpp_chn vicap_mpp_chn, k_mpp_chn vo_mpp_chn)
{
    k_s32 ret;

    ret = kd_mpi_sys_unbind(&vicap_mpp_chn, &vo_mpp_chn);
    if (ret) {
        printf("kd_mpi_sys_unbind failed:0x%x\n", ret);
    }
    return;
}

int sample_sys_bind_init(void)
{
    k_s32 ret = 0;
    k_mpp_chn vicap_mpp_chn;
    k_mpp_chn vo_mpp_chn;
    vicap_mpp_chn.mod_id = K_ID_VI;
    vicap_mpp_chn.dev_id = VICAP_DEV_ID_0;
    vicap_mpp_chn.chn_id = VICAP_CHN_ID_0;

    vo_mpp_chn.mod_id = K_ID_VO;
    vo_mpp_chn.dev_id = K_VO_DISPLAY_DEV_ID;
    vo_mpp_chn.chn_id = K_VO_DISPLAY_CHN_ID1;

    ret = kd_mpi_sys_bind(&vicap_mpp_chn, &vo_mpp_chn);
    if (ret) {
        printf("kd_mpi_sys_unbind failed:0x%x\n", ret);
    }
    return ret;
}

int sample_vb_init(void)
{
    k_s32 ret;
    k_u32 rtsp_venc_frame_size;
    k_u32 rtsp_venc_stream_size;

    memset(&config, 0, sizeof(config));
    config.max_pool_cnt = 64;

    config.comm_pool[0].blk_cnt = 5;
    config.comm_pool[0].mode = VB_REMAP_MODE_NOCACHE;
    config.comm_pool[0].blk_size = VICAP_ALIGN_UP((ISP_CHN0_WIDTH * ISP_CHN0_HEIGHT * 3 / 2), VICAP_ALIGN_1K);
    //gdma
    // config.comm_pool[1].blk_cnt = 4;
    // config.comm_pool[1].blk_size = ISP_OUT_HEIGHT*ISP_OUT_WIDTH*3;
    // config.comm_pool[1].mode = VB_REMAP_MODE_NOCACHE;
    //VB for RGB888 output
    config.comm_pool[1].blk_cnt = 5;
    config.comm_pool[1].mode = VB_REMAP_MODE_NOCACHE;
    config.comm_pool[1].blk_size = VICAP_ALIGN_UP((ISP_CHN1_HEIGHT * ISP_CHN1_WIDTH * 3 ), VICAP_ALIGN_1K);

    /*
     * Reserve dedicated pools for the little-core RTSP encode path.
     * Preview/AI has already consumed the original common pools, so without
     * these extra blocks kd_mapi_venc_init() fails with "no blk".
     */
    rtsp_venc_frame_size = VICAP_ALIGN_UP((1280 * 720 * 3 / 2), 0x1000);
    rtsp_venc_stream_size = VICAP_ALIGN_UP((1280 * 720 / 2), 0x1000);

    config.comm_pool[2].blk_cnt = 8;
    config.comm_pool[2].mode = VB_REMAP_MODE_NOCACHE;
    config.comm_pool[2].blk_size = rtsp_venc_frame_size;

    config.comm_pool[3].blk_cnt = 30;
    config.comm_pool[3].mode = VB_REMAP_MODE_NOCACHE;
    config.comm_pool[3].blk_size = rtsp_venc_stream_size;

    ret = kd_mpi_vb_set_config(&config);
    if (ret) {
        printf("vb_set_config failed ret:%d\n", ret);
        return ret;
    }

    k_vb_supplement_config supplement_config;
    memset(&supplement_config, 0, sizeof(supplement_config));
    supplement_config.supplement_config |= VB_SUPPLEMENT_JPEG_MASK;

    ret = kd_mpi_vb_set_supplement_config(&supplement_config);
    if (ret) {
        printf("vb_set_supplement_config failed ret:%d\n", ret);
        return ret;
    }
    ret = kd_mpi_vb_init();
    if (ret) {
        printf("vb_init failed ret:%d\n", ret);
    }
    return ret;
}

int sample_vivcap_init( void )
{
    k_s32 ret = 0;
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    sensor_type = GC2093_MIPI_CSI2_1920X1080_30FPS_10BIT_LINEAR;
#else
    sensor_type = IMX335_MIPI_2LANE_RAW12_2592X1944_30FPS_LINEAR;
#endif
    vicap_dev = VICAP_DEV_ID_0;

    memset(&sensor_info, 0, sizeof(k_vicap_sensor_info));
    ret = kd_mpi_vicap_get_sensor_info(sensor_type, &sensor_info);
    if (ret) {
        printf("sample_vicap, the sensor type not supported!\n");
        return ret;
    }

    memset(&dev_attr, 0, sizeof(k_vicap_dev_attr));
    dev_attr.acq_win.h_start = 0;
    dev_attr.acq_win.v_start = 0;
    dev_attr.acq_win.width = ISP_INPUT_WIDTH;
    dev_attr.acq_win.height = ISP_INPUT_HEIGHT;
    dev_attr.mode = VICAP_WORK_ONLINE_MODE;

    dev_attr.pipe_ctrl.data = 0xFFFFFFFF;
    dev_attr.pipe_ctrl.bits.af_enable = 0;
    dev_attr.pipe_ctrl.bits.ahdr_enable = 0;
    dev_attr.pipe_ctrl.bits.dnr3_enable = 0;

    dev_attr.cpature_frame = 0;
    memcpy(&dev_attr.sensor_info, &sensor_info, sizeof(k_vicap_sensor_info));

    ret = kd_mpi_vicap_set_dev_attr(vicap_dev, dev_attr);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_set_dev_attr failed.\n");
        return ret;
    }

    memset(&chn_attr, 0, sizeof(k_vicap_chn_attr));
    //set chn0 output yuv
    chn_attr.out_win.h_start = 0;
    chn_attr.out_win.v_start = 0;
    chn_attr.out_win.width = ISP_CHN0_WIDTH;
    chn_attr.out_win.height = ISP_CHN0_HEIGHT;

    chn_attr.crop_win = dev_attr.acq_win;
#if !defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    chn_attr.crop_win.h_start = ISP_CROP_W_OFFSET;
    chn_attr.crop_win.v_start = ISP_CROP_H_OFFSET;
    chn_attr.crop_win.width = ISP_CHN0_WIDTH;
    chn_attr.crop_win.height = ISP_CHN0_HEIGHT;
#endif
    chn_attr.crop_enable = K_FALSE;
    chn_attr.scale_enable = K_FALSE;
    // chn_attr.dw_enable = K_FALSE;
    chn_attr.chn_enable = K_TRUE;
    chn_attr.pix_format = PIXEL_FORMAT_YVU_PLANAR_420;
    chn_attr.scale_win = chn_attr.out_win;
    chn_attr.buffer_num = VICAP_MAX_FRAME_COUNT;//at least 3 buffers for isp
    chn_attr.buffer_size = VICAP_ALIGN_UP((ISP_CHN0_WIDTH * ISP_CHN0_HEIGHT * 3 / 2), VICAP_ALIGN_1K);
    vicap_chn = VICAP_CHN_ID_0;

    ret = kd_mpi_vicap_set_chn_attr(vicap_dev, vicap_chn, chn_attr);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_set_chn_attr failed.\n");
        return ret;
    }

    // set chn1 output for AI or RTSP shared path
    chn_attr.out_win.h_start = 0;
    chn_attr.out_win.v_start = 0;
    chn_attr.out_win.width = ISP_CHN1_WIDTH ;
    chn_attr.out_win.height = ISP_CHN1_HEIGHT;

    chn_attr.crop_win = dev_attr.acq_win;
#if !defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    chn_attr.crop_win.h_start = ISP_CROP_W_OFFSET;
    chn_attr.crop_win.v_start = ISP_CROP_H_OFFSET;
    chn_attr.crop_win.width = ISP_CHN0_WIDTH;
    chn_attr.crop_win.height = ISP_CHN0_HEIGHT;
#endif

    chn_attr.scale_win = chn_attr.out_win;
    chn_attr.crop_enable = K_FALSE;
    chn_attr.scale_enable = K_FALSE;
    chn_attr.chn_enable = K_TRUE;
    chn_attr.buffer_num = VICAP_MAX_FRAME_COUNT;//at least 3 buffers for isp
    if (lawrec_ai_pipeline_enabled()) {
        chn_attr.pix_format = PIXEL_FORMAT_RGB_888_PLANAR;
        chn_attr.buffer_size =
            VICAP_ALIGN_UP((ISP_CHN1_HEIGHT * ISP_CHN1_WIDTH * 3), VICAP_ALIGN_1K);
    } else {
        /*
         * Current LCKFB runtime disables the AI pipeline, so dedicate chn1 to
         * the little-core RTSP encode feed. This path is known to produce
         * frames, unlike the tentative chn2 route in the current online mode.
         */
        chn_attr.pix_format = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
        chn_attr.buffer_size =
            VICAP_ALIGN_UP((ISP_CHN1_HEIGHT * ISP_CHN1_WIDTH * 3 / 2), VICAP_ALIGN_1K);
    }

    ret = kd_mpi_vicap_set_chn_attr(vicap_dev, VICAP_CHN_ID_1, chn_attr);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_set_chn_attr failed.\n");
        return ret;
    }

    // set chn2 output yuv420 for little-core RTSP encode path
    chn_attr.out_win.h_start = 0;
    chn_attr.out_win.v_start = 0;
    chn_attr.out_win.width = 1280;
    chn_attr.out_win.height = 720;
    chn_attr.crop_win = dev_attr.acq_win;
#if !defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    chn_attr.crop_win.h_start = ISP_CROP_W_OFFSET;
    chn_attr.crop_win.v_start = ISP_CROP_H_OFFSET;
    chn_attr.crop_win.width = 1280;
    chn_attr.crop_win.height = 720;
#endif
    chn_attr.scale_win = chn_attr.out_win;
    chn_attr.crop_enable = K_FALSE;
    chn_attr.scale_enable = K_FALSE;
    chn_attr.chn_enable = K_TRUE;
    chn_attr.pix_format = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    chn_attr.buffer_num = 6;
    chn_attr.buffer_size = VICAP_ALIGN_UP((1280 * 720 * 3 / 2), 0x1000);

    ret = kd_mpi_vicap_set_chn_attr(vicap_dev, VICAP_CHN_ID_2, chn_attr);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_set_chn_attr chn2 failed.\n");
        return ret;
    }
    printf("[lawrec] build_tag=%s\n", LAWREC_BUILD_TAG);
    printf("[lawrec] sensor_type=%d, parse_mode=VICAP_DATABASE_PARSE_XML_JSON, face_db=%d, face_data=0x%08x size=0x%x\n",
           sensor_type, LAWREC_ENABLE_FACE_DB, (unsigned int)LAWREC_FACE_DATA_BASE, (unsigned int)LAWREC_FACE_DATA_SIZE);

    ret = kd_mpi_vicap_set_database_parse_mode(vicap_dev, VICAP_DATABASE_PARSE_XML_JSON);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_set_database_parse_mode failed.\n");
        return ret;
    }

    ret = kd_mpi_vicap_init(vicap_dev);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_init failed.\n");
        return ret;
        // goto err_exit;
    }
    ret = kd_mpi_vicap_start_stream(vicap_dev);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_init failed.\n");
        return ret;
        // goto err_exit;
    }
    return ret;
}

static void *exit_app(void *arg)
{
    int tty_fd = -1;
    char ch = 0;

    printf("press 'q' to exit application!!\n");
    fflush(stdout);

    tty_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK);
    if (tty_fd >= 0)
    {
        while (app_run)
        {
            int ret = read(tty_fd, &ch, 1);
            if (ret == 1 && (ch == 'q' || ch == 'Q'))
            {
                printf("\n[lawrec-big] quit requested from tty\n");
                fflush(stdout);
                app_run = false;
                break;
            }
            if (ret < 0 && errno != EAGAIN && errno != EINTR)
            {
                printf("[lawrec-big] tty read failed errno=%d\n", errno);
                fflush(stdout);
                break;
            }
            usleep(10000);
        }
        close(tty_fd);
        return NULL;
    }

    while (app_run)
    {
        int input = getchar();
        if (input == 'q' || input == 'Q')
        {
            printf("\n[lawrec-big] quit requested from stdin\n");
            fflush(stdout);
            app_run = false;
            break;
        }
        if (input == EOF)
        {
            clearerr(stdin);
            usleep(10000);
            continue;
        }
        usleep(10000);
    }

    return NULL;
}

void draw_result(cv::Mat& src_img,box_t& bbox,FaceMaskInfo& result, bool pic_mode,bool is_label,std::vector<cv::Point2f> points1)
{
    int src_w = src_img.cols;
    int src_h = src_img.rows;
    int max_src_size = std::max(src_w,src_h);

    char text[30];
    // sprintf(text, "%.2f",result.score);
	sprintf(text, "%s",result.label.c_str());

    if(pic_mode)
    {
        cv::rectangle(src_img, cv::Rect(bbox.x, bbox.y , bbox.w, bbox.h), cv::Scalar(255, 255, 255), 2, 2, 0);
        if(is_label == true)
        {
            if(result.score<mask_thresh_)
                cv::putText(src_img, text , {bbox.x,std::max(int(bbox.y-10),0)}, cv::FONT_HERSHEY_COMPLEX, 0.6, cv::Scalar(255, 0, 0), 1, 8, 0);
            else
                cv::putText(src_img, text , {bbox.x,std::max(int(bbox.y-10),0)}, cv::FONT_HERSHEY_COMPLEX, 0.6, cv::Scalar(0, 0, 255), 1, 8, 0);
            is_label = false;
        }

    }
    else
    {
        int x = bbox.x;
        int y = bbox.y;
        int w = bbox.w;
        int h = bbox.h;
        cv::rectangle(src_img, cv::Rect(x, y , w, h), cv::Scalar(255,255, 255, 255), 2, 2, 0);
        for(int i = 0; i < points1.size();i++)
        {
            // printf(" circle %f %f \n",points1[i].x,points1[i].y);
            cv::circle(src_img,points1[i],3,cv::Scalar(255,255, 0, 0), 3);
        }

        if(is_label == true)
        {
            if(result.score<mask_thresh_)
                cv::putText(src_img, text, {std::max(int(x),0), std::max(int(y - 10),0)}, cv::FONT_HERSHEY_COMPLEX, 2.5, cv::Scalar(255,255, 0, 0), 4, 8, 0);
            else
                cv::putText(src_img, text, {std::max(int(x-10),0), std::max(int(y - 10),0)}, cv::FONT_HERSHEY_COMPLEX, 2.5, cv::Scalar(255,0, 0, 255), 4, 8, 0);
            is_label = false;
        }


    }
    return;
}

void getFileNames(char *path, std::vector<std::string>& files)
{
    DIR *dir;
    struct dirent *ptr;
    dir = opendir("/sharefs/pic/");
    char pic_name[128];
    if(dir == NULL)
    {
        return;
    }
    while((ptr = readdir(dir)) != NULL)
    {
        if(strstr(ptr->d_name,".jpg"))
        {
            printf("ptr->d_name %s\n",ptr->d_name);
            files.push_back(ptr->d_name);
        }

    }
    closedir(dir);
    return;

}



int main(int argc, char *argv[])
{
    printf("[lawrec] build media-playback-dev " __DATE__ " " __TIME__ "\n");
    struct sigaction sa;
    k_s32 mapi_ret = kd_mapi_sys_init();
    printf("[lawrec-big] mapi server init ret=%d\n", mapi_ret);

    bool rtsp_requested = lawrec_rtsp_requested(argc, argv);

    if (!LAWREC_STAGE0_PREVIEW_ONLY && argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << " <retinaface.kmodel> <mbface.kmodel>" << std::endl;
        kd_mapi_sys_deinit();
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fun_sig;
    sigfillset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    k_u32 display_ms = 1000 / 33;
    int face_count = 32;
    size_t paddr = 0;
    void *vaddr = nullptr;
    DetectResult detect_result;
    std::vector<float> feature_result;
    std::vector<box_t> _draw_result;
    bool depth_pred = true;
    bool ir_pred = false;
    float score_max = 0;
    int score_index = 0;
    float score_threshold = 0.82f;
    bool is_label = false;
    bool preview_was_enabled = false;

    pthread_t vo_thread_handle;
    pthread_t exit_thread_handle;
    // pthread_t key_opreation_handle;
    pthread_t ipc_message_handle;

    size_t size = CHANNEL * ISP_CHN1_HEIGHT * ISP_CHN1_WIDTH;

    k_u32 pool_id;
    k_vb_pool_config pool_config;
    k_video_frame_info vf_info;
    void *pic_vaddr = NULL;
    int ret = kd_mpi_sys_mmz_alloc_cached(&paddr, &vaddr, "allocate", "anonymous", size);
    if (ret)
    {
        std::cerr << "physical_memory_block::allocate failed: ret = " << ret << ", errno = " << strerror(errno) << std::endl;
        std::abort();
    }
    if (lawrec_face_db_enabled())
    {
        read_mem_feature();
    }

    std::unique_ptr<MobileRetinaface> retinaface;
    if (lawrec_ai_pipeline_enabled())
    {
        retinaface = std::make_unique<MobileRetinaface>((const char*)argv[1], CHANNEL, ISP_CHN1_HEIGHT, ISP_CHN1_WIDTH);
    }
    std::unique_ptr<MobileFace> mf;
    if (lawrec_face_db_enabled())
    {
        mf = std::make_unique<MobileFace>((const char*)argv[2], CHANNEL, ISP_CHN1_HEIGHT, ISP_CHN1_WIDTH);
    }
    if (lawrec_start_ipc_server(&ipc_message_handle) != 0)
    {
        printf("[lawrec] start ipc server failed\n");
        goto app_error;
    }
    pthread_create(&exit_thread_handle, NULL, exit_app, NULL);
    // pthread_create(&key_opreation_handle, NULL, Get_KeyValue, NULL);
    // sample_vo_init();
    // sample_sys_bind_init();

    TEST_TIME(ret = sample_vb_init(), "sample_vb_init");
    if(ret) {
        goto vb_init_error;
    }

    pthread_create(&vo_thread_handle, NULL, sample_vo_thread, NULL);
    TEST_TIME(ret = sample_vivcap_init(),"sample_vicap_init");
    pthread_join(vo_thread_handle, NULL);
    if(ret) {
        goto vicap_init_error;
    }
    lawrec_preview_blank_init();
    g_preview.SetBackendReady(true);
    lawrec_preview_force_unbind();
    printf("[lawrec] preview backend ready, bind=%d enabled=%d\n",
           (int)g_preview.Bound(), (int)g_preview.Enabled());
    if (rtsp_requested)
    {
        if (lawrec_rtsp_start() != 0)
            printf("[lawrec] rtsp request init failed\n");
    }

    if (vicap_install_osd == 1)
    {
        memset(&pool_config, 0, sizeof(pool_config));
        pool_config.blk_size = VICAP_ALIGN_UP((osd_height * osd_width * 4), VICAP_ALIGN_1K);
        pool_config.blk_cnt = 1;
        pool_config.mode = VB_REMAP_MODE_NOCACHE;
        pool_id = kd_mpi_vb_create_pool(&pool_config); // osd0 - 3 argb 320 x 240
        g_pool_id = pool_id;
    }

    if (vicap_install_osd == 1)
    {
        memset(&vf_info, 0, sizeof(vf_info));
        vf_info.v_frame.width = osd_width;
        vf_info.v_frame.height = osd_height;
        vf_info.v_frame.stride[0] = osd_width;
        vf_info.v_frame.pixel_format = PIXEL_FORMAT_ARGB_8888;
        block = vo_insert_frame(&vf_info, &pic_vaddr);
    }

    if (LAWREC_STAGE0_PREVIEW_ONLY)
    {
        while (app_run)
        {
            usleep(10000);
        }
        goto app_exit;
    }

    while(app_run)
    {
        /* Keep preview idle cheap when UI has not requested camera output. */
        if (!g_preview.Enabled() && sensor_process == true)
        {
            if (preview_was_enabled)
                lawrec_clear_osd_frame(pic_vaddr, &vf_info);
            preview_was_enabled = false;
            usleep(10000);
            continue;
        }
        preview_was_enabled = g_preview.Enabled();

        if(sensor_process == true)
        {
            /* Live camera path: preview/AI/OSD all currently converge here. */
            uint64_t start_time,end_time;

            int num = 0;
            memset(&dump_info, 0 , sizeof(k_video_frame_info));
            if (!app_run)
            {
                break;
            }

            if (!lawrec_ai_pipeline_enabled())
            {
                lawrec_clear_osd_frame(pic_vaddr, &vf_info);
                usleep(10000);
                continue;
            }

            ret = kd_mpi_vicap_dump_frame(vicap_dev, VICAP_CHN_ID_1, VICAP_DUMP_YUV, &dump_info, 1000);
            if (ret) {
                quit.store(false);
                printf("sample_vicap...kd_mpi_vicap_dump_frame failed.\n");
                break;
            }
            box_t face_box;

            auto vbvaddr = kd_mpi_sys_mmap(dump_info.v_frame.phys_addr[0], size);
            if (vbvaddr == nullptr)
            {
                printf("[lawrec] mmap dump frame failed\n");
                ret = kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
                if (ret) {
                    printf("sample_vicap...kd_mpi_vicap_dump_release failed.\n");
                }
                continue;
            }
            detect_result.boxes.clear();
            detect_result.landmarks.clear();
            _draw_result.clear();

            // run kpu
            // TEST_BOOT_TIME_TRIGER();
            start_time = perf_get_smodecycles();
            retinaface->run(reinterpret_cast<uintptr_t>(vbvaddr), reinterpret_cast<uintptr_t>(dump_info.v_frame.phys_addr[0]));
            end_time = perf_get_smodecycles();
            // get face boxes
            detect_result = retinaface->get_result();
            cv::Mat osd_frame;
            if (vicap_install_osd == 1)
            {
                osd_frame = cv::Mat(osd_height, osd_width, CV_8UC4, cv::Scalar(0, 0, 0, 0));
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
                cv::rotate(osd_frame, osd_frame, cv::ROTATE_90_COUNTERCLOCKWISE);
#endif
            }
            size_t draw_boxes = 0;
            printf("[lawrec] detect boxes=%zu\n", detect_result.boxes.size());
            if (!detect_result.boxes.empty())
            {
                const auto &box0 = detect_result.boxes[0];
                printf("[lawrec] box0=(%d,%d)-(%d,%d)\n", box0.x1, box0.y1, box0.x2, box0.y2);
            }

            if (detect_result.boxes.size() < face_count)
            {
                for (size_t i = detect_result.boxes.size(); i < (size_t)face_count; ++i)
                {
                    vo_frame.draw_en = 0;
                    vo_frame.frame_num = i + 1;
                    kd_mpi_vo_draw_frame(&vo_frame);
                }
            }

            for (size_t i = 0; i < detect_result.boxes.size(); i++)
            {
                auto box = detect_result.boxes[i];
                if (!is_valid_detect_box(box))
                {
                    continue;
                }
                draw_boxes += 1;
                FaceMaskInfo fm_result;
                cv::Point2f point;
                std::vector<cv::Point2f> points1;
                map_detect_box_to_display(box, &vo_frame);
                face_box = map_detect_box_to_osd(box);
                for(int t = 0;t<5;t++)
                {
                    point = map_landmark_to_osd(
                        detect_result.landmarks[i].points[2 * t + 0],
                        detect_result.landmarks[i].points[2 * t + 1]);
                    points1.push_back(point);
                }

                if (mf)
                {
                    mf->update_ai2d_config(detect_result.landmarks[i]);
                    mf->run(reinterpret_cast<uintptr_t>(vbvaddr), reinterpret_cast<uintptr_t>(dump_info.v_frame.phys_addr[0]));
                    feature_result = mf->get_result();

                    if(key_press == true)
                    {
                        pthread_mutex_lock(&mutex);
                        printf("mem_feature_data->count = %d\n",mem_feature_data->count);
                        memcpy(mem_feature_data->feature_db_data[mem_feature_data->count].feature, feature_result.data(), feature_result.size() * sizeof(feature_result[0]));
                        sprintf(mem_feature_data->feature_db_data[mem_feature_data->count].name,"%s",name.c_str());
                        mem_feature_data->count++;
                        l2normalize_feature_db(mem_feature_data,mem_feature_data->count);
                        pthread_mutex_unlock(&mutex);
                        ipc_send_thread(MSG_CMD_FEATURE_SAVE);
                        key_press = false;
                    }
                    pthread_mutex_lock(&mutex);
                    score_index = calulate_score(mem_feature_data->count, feature_result, &score_max);
                    pthread_mutex_unlock(&mutex);
                    if (score_max >= score_threshold)
                    {
                        fm_result.label = mem_feature_data->feature_db_data[score_index].name;
                        is_label = true;
                    }
                }

                if (vicap_install_osd == 1)
                {
                    draw_result(osd_frame,face_box,fm_result,false,is_label,points1);
                }

#if !defined(CONFIG_BOARD_K230_CANMV_LCKFB)
                vo_frame.draw_en = 1;
                vo_frame.frame_num = i + 1;
                ret = kd_mpi_vo_draw_frame(&vo_frame);
                if (ret != 0)
                {
                    printf("[lawrec] draw_frame failed ret=%d idx=%zu frame=%u box=(%u,%u)-(%u,%u)\n",
                           ret,
                           i,
                           vo_frame.frame_num,
                           vo_frame.line_x_start,
                           vo_frame.line_y_start,
                           vo_frame.line_x_end,
                           vo_frame.line_y_end);
                }
                else if (i == 0)
                {
                    printf("[lawrec] draw_frame ok frame=%u box=(%u,%u)-(%u,%u)\n",
                           vo_frame.frame_num,
                           vo_frame.line_x_start,
                           vo_frame.line_y_start,
                           vo_frame.line_x_end,
                           vo_frame.line_y_end);
                }
#endif
            }
            printf("[lawrec] draw boxes=%zu\n", draw_boxes);
            face_count = detect_result.boxes.size();
            if (vicap_install_osd == 1)
            {
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
                cv::rotate(osd_frame, osd_frame, cv::ROTATE_90_CLOCKWISE);
#endif
                memcpy(pic_vaddr, osd_frame.data, osd_width * osd_height * 4);
                ret = kd_mpi_vo_chn_insert_frame(osd_id + 3, &vf_info);
                if (ret != 0)
                {
                    printf("[lawrec] osd insert failed ret=%d\n", ret);
                }
            }
            kd_mpi_sys_munmap(vbvaddr, size);
            ret = kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
            if (ret) {
                printf("sample_vicap...kd_mpi_vicap_dump_release failed.\n");
            }
        }
        else
        {
            /* Historical import path from /sharefs/pic, kept only for compatibility. */
            if (!lawrec_ai_pipeline_enabled())
            {
                sensor_process = true;
                continue;
            }

            cv::Mat ori_img_R = cv::Mat(ISP_CHN1_HEIGHT, ISP_CHN1_WIDTH, CV_8UC1, vaddr);
            cv::Mat ori_img_G = cv::Mat(ISP_CHN1_HEIGHT, ISP_CHN1_WIDTH, CV_8UC1, vaddr + ISP_CHN1_HEIGHT * ISP_CHN1_WIDTH);
            cv::Mat ori_img_B = cv::Mat(ISP_CHN1_HEIGHT, ISP_CHN1_WIDTH, CV_8UC1, vaddr + ISP_CHN1_HEIGHT * ISP_CHN1_WIDTH * 2);
            std::vector<std::string> files;
            std::vector<cv::Mat> input_channels;

            getFileNames(dir_name,files);
            for(int i = 0;i < files.size();i++)
            {
                char pic_path[100];
                sprintf(pic_path,"/sharefs/pic/%s",files[i].c_str());
                cv::Mat img = cv::imread(pic_path);
                cv::resize(img, img, cv::Size(720, 1280));
                cv::split(img, input_channels);
                memcpy(ori_img_R.data, input_channels[2].data, ISP_CHN1_HEIGHT * ISP_CHN1_WIDTH * sizeof(char));
                memcpy(ori_img_G.data, input_channels[1].data, ISP_CHN1_HEIGHT * ISP_CHN1_WIDTH * sizeof(char));
                memcpy(ori_img_B.data, input_channels[0].data, ISP_CHN1_HEIGHT * ISP_CHN1_WIDTH * sizeof(char));
                retinaface->run(reinterpret_cast<uintptr_t>(vaddr), reinterpret_cast<uintptr_t>(paddr));
                detect_result = retinaface->get_result();
                if(detect_result.boxes.size() > 0 && lawrec_face_db_enabled() && mf)
                {
                    mf->update_ai2d_config(detect_result.landmarks[0]);
                    mf->run(reinterpret_cast<uintptr_t>(vaddr), reinterpret_cast<uintptr_t>(paddr));
                    feature_result = mf->get_result();
                    pthread_mutex_lock(&mutex);
                    memcpy(mem_feature_data->feature_db_data[mem_feature_data->count].feature, feature_result.data(), feature_result.size() * sizeof(feature_result[0]));
                    sprintf(mem_feature_data->feature_db_data[mem_feature_data->count].name,"%s",files[i].erase(files[i].find(".jpg"),4).c_str());
                    mem_feature_data->count++;
                    l2normalize_feature_db(mem_feature_data,mem_feature_data->count);
                    pthread_mutex_unlock(&mutex);
                    ipc_send_thread(MSG_CMD_FEATURE_SAVE);
                }
                else if (lawrec_face_db_enabled())
                {
                    ipc_send_thread(MSG_CMD_ERROR);
                }

            }
            sensor_process = true;
        }
    }
app_exit:
    g_rtsp_compat.StopRequested();
    if (lawrec_face_db_enabled())
    {
        if (mem_fd >= 0)
        {
            close(mem_fd);
        }
        if (mem_feature_data != nullptr)
        {
            munmap(mem_feature_data, LAWREC_FACE_DATA_SIZE);
        }
    }
    pthread_join(exit_thread_handle, NULL);
    printf("[lawrec-big] exit thread joined\n");
    fflush(stdout);
    // pthread_join(key_opreation_handle, NULL);
    if (!LAWREC_STAGE0_PREVIEW_ONLY)
    {
        printf("[lawrec-big] ipc shutdown begin\n");
        fflush(stdout);
        kd_ipcmsg_disconnect(s32Id1);
        pthread_join(ipc_message_handle, NULL);
        kd_ipcmsg_del_service(LAWREC_IPC_SERVICE_NAME);
        printf("[lawrec-big] ipc shutdown done\n");
        fflush(stdout);
    }
    if (vicap_install_osd == 1)
    {
        vo_osd_release_block();
        kd_mpi_sys_munmap(pic_vaddr, osd_width * osd_height * 4);
    }
    for(size_t i = 0;i < detect_result.boxes.size();i++)
    {
        vo_frame.draw_en = 0;
        vo_frame.frame_num = i + 1;
        kd_mpi_vo_draw_frame(&vo_frame);
    }
    detect_result.boxes.clear();
    printf("[lawrec-big] vicap stop begin\n");
    fflush(stdout);
    ret = kd_mpi_vicap_stop_stream(vicap_dev);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_init failed.\n");
    }
    ret = kd_mpi_vicap_deinit(vicap_dev);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_deinit failed.\n");
        return ret;
    }
    lawrec_preview_blank_deinit();
    printf("[lawrec-big] vicap stop done\n");
    fflush(stdout);

vicap_init_error:
    kd_mpi_vo_disable_video_layer(K_VO_LAYER1);


    k_mpp_chn vicap_mpp_chn;
    k_mpp_chn vo_mpp_chn;
    vicap_mpp_chn.mod_id = K_ID_VI;
    vicap_mpp_chn.dev_id = vicap_dev;
    vicap_mpp_chn.chn_id = vicap_chn;

    vo_mpp_chn.mod_id = K_ID_VO;
    vo_mpp_chn.dev_id = K_VO_DISPLAY_DEV_ID;
    vo_mpp_chn.chn_id = K_VO_DISPLAY_CHN_ID1;

    sample_vicap_unbind_vo(vicap_mpp_chn, vo_mpp_chn);

    usleep(1000 * display_ms);
dma_init_error:
    printf("[lawrec-big] vb exit begin\n");
    fflush(stdout);
    ret = kd_mpi_vb_exit();
    if (ret) {
        printf("fastboot_app, kd_mpi_vb_exit failed.\n");
        return ret;
    }
    printf("[lawrec-big] vb exit done\n");
    fflush(stdout);

app_error:
    printf("[lawrec-big] mapi sys deinit begin\n");
    fflush(stdout);
    kd_mapi_sys_deinit();
    printf("[lawrec-big] app exit done\n");
    fflush(stdout);
    return 0;

vb_init_error:
    kd_mapi_sys_deinit();
    return 0;
}
