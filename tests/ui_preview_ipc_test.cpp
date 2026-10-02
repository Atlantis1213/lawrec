// Production IPC consumer and control, without threads, hardware or LVGL drawing.
#include "../little/src/ui/src/msg_proc.cpp"
#include <cassert>
#include <vector>

struct sent_request { uint32_t command, sequence; };
static std::vector<sent_request> sent;
static int send_error, rtsp_state, record_state, rtsp_stops, record_stops;
static bool allocation_failure, back_pending, playback, main_screen;
static std::string preview_text;
static unsigned media_mask;
static int recovery_result, recovery_results;
unsigned lawrec_media_owner_mask(void) { return media_mask; }
extern "C" void scr_maintenance_recovery_result(int result) { recovery_result=result; ++recovery_results; }

extern "C" k_bool kd_ipcmsg_is_connect(k_s32 id) { return id == ipcmsg_handle.load() && id >= 0 ? K_TRUE : K_FALSE; }
extern "C" k_ipcmsg_message_t *kd_ipcmsg_create_message(k_u32, k_u32 command, const void *body, k_u32 size)
{
    if (allocation_failure) return nullptr;
    auto *message = static_cast<k_ipcmsg_message_t *>(calloc(1, sizeof(k_ipcmsg_message_t)));
    assert(message);
    message->u32CMD = command; message->u32BodyLen = size;
    message->pBody = malloc(size); assert(message->pBody);
    memcpy(message->pBody, body, size);
    return message;
}
extern "C" void kd_ipcmsg_destroy_message(k_ipcmsg_message_t *message)
{
    free(message->pBody); free(message);
}
extern "C" k_s32 kd_ipcmsg_send_only(k_s32 id, k_ipcmsg_message_t *message)
{
    assert(id == ipcmsg_handle.load());
    lawrec_preview_wire_t wire{};
    if (message->u32BodyLen == sizeof(wire)) memcpy(&wire, message->pBody, sizeof(wire));
    sent.push_back({message->u32CMD, wire.sequence});
    return send_error;
}

extern "C" int lawrec_rtsp_get_state(void) { return rtsp_state; }
extern "C" int lawrec_record_get_state(void) { return record_state; }
extern "C" int lawrec_rtsp_stop_async(void) { ++rtsp_stops; rtsp_state = LAWREC_RTSP_STATE_STOPPING; return 0; }
extern "C" int lawrec_record_stop_async(void) { ++record_stops; record_state = LAWREC_RECORD_STATE_STOPPING; return 0; }
extern "C" int lawrec_playback_active(void) { return playback; }
extern "C" int lawrec_playback_start(const char *) { playback = true; return 0; }
extern "C" void lawrec_playback_stop(void) { playback = false; }
extern "C" int feature_db_save(uint32_t, uint32_t) { return 0; }
extern "C" void scr_main_display_result(int8_t) {}
extern "C" void scr_main_set_status(const char *, lv_color_t) {}
extern "C" void scr_preview_set_status(const char *text, lv_color_t) { preview_text = text; }
extern "C" int scr_preview_is_back_pending(void) { return back_pending; }
extern "C" void scr_preview_clear_back_pending(void) { back_pending = false; }
extern "C" void jump_to_scr_main(void) { main_screen = true; }
#define BUTTON_STUB(name) extern "C" void name(void) {}
BUTTON_STUB(scr_preview_set_rtsp_button_unavailable)
BUTTON_STUB(scr_preview_set_rtsp_button_idle)
BUTTON_STUB(scr_preview_set_rtsp_button_starting)
BUTTON_STUB(scr_preview_set_rtsp_button_live)
BUTTON_STUB(scr_preview_set_rtsp_button_stopping)
BUTTON_STUB(scr_preview_set_record_button_unavailable)
BUTTON_STUB(scr_preview_set_record_button_idle)
BUTTON_STUB(scr_preview_set_record_button_starting)
BUTTON_STUB(scr_preview_set_record_button_live)
BUTTON_STUB(scr_preview_set_record_button_stopping)
#undef BUTTON_STUB

static void response(uint32_t command, uint32_t sequence, int result = 0)
{
    lawrec_preview_wire_t wire{LAWREC_PREVIEW_WIRE_VERSION, sequence, result};
    k_ipcmsg_message_t message{};
    message.u32CMD = command; message.pBody = &wire; message.u32BodyLen = sizeof(wire);
    msg_recv(ipcmsg_handle.load(), &message);
    ui_msg_proc();
}

static void reset()
{
    ui_msg_t *message;
    while (!ui_msg_get(&message)) ui_msg_free(message);
    pending_preview = 0; back_pending = false; main_screen = false;
    display_query_pending = false; display_sync_generation = ipc_generation.load();
    display_query_retry = std::chrono::steady_clock::time_point();
    rtsp_state = LAWREC_RTSP_STATE_IDLE; record_state = LAWREC_RECORD_STATE_IDLE;
    send_error = 0; allocation_failure = false; playback = false;
    media_mask=0;
    lawrec_control_note_display_status(1, 0, 0); sent.clear();
}

static void display_response(uint32_t sequence, unsigned ready, unsigned enabled,
                             unsigned bound, unsigned remote_playback = 0)
{
    lawrec_display_status_t status{LAWREC_DISPLAY_STATUS_VERSION, sequence, 0,
                                  ready, enabled, bound, remote_playback};
    k_ipcmsg_message_t message{};
    message.u32CMD = MSG_CMD_DISPLAY_STATUS; message.pBody = &status;
    message.u32BodyLen = sizeof(status);
    msg_recv(ipcmsg_handle.load(), &message); ui_msg_proc();
}

int main()
{
    lawrec_control_set_log_path("/dev/null");
    assert(!lawrec_control_init());
    assert(lawrec_control_playback_start("test.mp4") == -EBUSY);
    ipcmsg_handle = 42; ipc_status = 1; ipc_generation = 1;
    ui_msg_proc();
    reset();

    // Back during ENTER must wait, then emit exactly one EXIT without a status event.
    assert(!msg_send_cmd(MSG_CMD_PREVIEW_ENTER));
    const uint32_t enter = preview_sequence;
    lawrec_preview_wire_t bad_wire{LAWREC_PREVIEW_WIRE_VERSION + 1, enter, 0};
    k_ipcmsg_message_t bad_message{};
    bad_message.u32CMD = MSG_CMD_PREVIEW_ENTER_RESULT;
    bad_message.pBody = &bad_wire; bad_message.u32BodyLen = sizeof(bad_wire);
    msg_recv(42, &bad_message); ui_msg_proc();
    assert(pending_preview == MSG_CMD_PREVIEW_ENTER && !lawrec_control_is_preview_active());
    bad_message.u32BodyLen = 1;
    msg_recv(42, &bad_message); ui_msg_proc();
    assert(pending_preview == MSG_CMD_PREVIEW_ENTER);
    back_pending = true;
    assert(msg_send_cmd(MSG_CMD_PREVIEW_EXIT) == -EBUSY && sent.size() == 1);
    for (int i = 0; i < 10; ++i) ui_msg_proc();
    assert(sent.size() == 1);
    response(MSG_CMD_PREVIEW_ENTER_RESULT, enter);
    assert(sent.size() == 2 && sent.back().command == MSG_CMD_PREVIEW_EXIT);
    assert(!lawrec_control_is_preview_active() && lawrec_control_preview_needs_close());
    assert(lawrec_control_playback_start("test.mp4") == -EBUSY);
    response(MSG_CMD_PREVIEW_ENTER_RESULT, enter); // stale ENTER cannot unblock consumers
    assert(pending_preview == MSG_CMD_PREVIEW_EXIT);
    response(MSG_CMD_PREVIEW_EXIT_RESULT, preview_sequence);
    assert(main_screen && !back_pending && !lawrec_control_preview_needs_close());
    assert(!lawrec_control_playback_start("test.mp4"));
    reset();

    // Exit only after BOTH local consumers finish, not after the first status update.
    lawrec_control_note_preview_request(1); lawrec_control_note_preview_result(1);
    rtsp_state = LAWREC_RTSP_STATE_STOPPING; record_state = LAWREC_RECORD_STATE_RECORDING;
    back_pending = true; ui_msg_proc(); assert(sent.empty());
    rtsp_state = LAWREC_RTSP_STATE_IDLE; ui_msg_proc(); assert(sent.empty());
    record_state = LAWREC_RECORD_STATE_IDLE; ui_msg_proc(); assert(sent.size() == 1);
    response(MSG_CMD_PREVIEW_EXIT_RESULT, preview_sequence, -EIO);
    assert(!main_screen && !back_pending && lawrec_control_preview_needs_close());
    assert(lawrec_control_playback_start("test.mp4") == -EBUSY);
    back_pending = true; ui_msg_proc(); assert(sent.size() == 2);
    response(MSG_CMD_PREVIEW_EXIT_RESULT, preview_sequence);
    assert(main_screen && !lawrec_control_preview_needs_close());
    reset();

    // Timeout is applied immediately, even when the result queue is full.
    assert(!msg_send_cmd(MSG_CMD_PREVIEW_ENTER));
    const uint32_t timed_out = preview_sequence;
    int8_t ok = 0;
    for (int i = 0; i < UI_MSG_QUEUE_MAX_COUNT; ++i) assert(!common_msg_proc_helper(UI_CMD_PING_RESULT, &ok));
    preview_deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    ui_msg_proc();
    assert(!pending_preview && !lawrec_control_is_preview_active() && lawrec_control_preview_needs_close());
    while (!msg_mgt.msg_q.empty()) ui_msg_proc();
    response(MSG_CMD_PREVIEW_ENTER_RESULT, timed_out);
    assert(!lawrec_control_is_preview_active());
    back_pending = true; ui_msg_proc();
    preview_deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    ui_msg_proc(); assert(!pending_preview && !back_pending && !main_screen);
    for (int i = 0; i < 10; ++i) ui_msg_proc();
    assert(sent.size() == 2); // no infinite automatic EXIT retries
    reset();

    // Offline failure is synchronous and does not fabricate an idle backend.
    ipc_status = -1; ipcmsg_handle = -1; ++ipc_generation; ui_msg_proc();
    assert(msg_send_cmd(MSG_CMD_PREVIEW_ENTER) == -ENOTCONN);
    assert(!pending_preview && lawrec_control_preview_needs_close());
    assert(lawrec_control_playback_start("test.mp4") == -EBUSY);
    ipcmsg_handle = 43; ipc_status = 1; ++ipc_generation; ui_msg_proc();
    back_pending = true; ui_msg_proc();
    response(MSG_CMD_PREVIEW_EXIT_RESULT, preview_sequence);
    assert(!lawrec_control_preview_needs_close());
    reset();

    // A reconnect hidden between UI polls invalidates a queued old success.
    assert(!msg_send_cmd(MSG_CMD_PREVIEW_ENTER));
    assert(!common_msg_proc_helper(UI_CMD_PREVIEW_ENTER_RESULT, &ok, preview_sequence));
    ipcmsg_handle = 44; ipc_generation += 2; // status remains 1
    ui_msg_proc();
    assert(!pending_preview && !lawrec_control_is_preview_active() && lawrec_control_preview_needs_close());
    assert(lawrec_control_playback_start("test.mp4") == -EBUSY);
    back_pending = true; ui_msg_proc(); response(MSG_CMD_PREVIEW_EXIT_RESULT, preview_sequence);
    reset();

    // Immediate SDK send/allocation errors cannot strand the page in pending.
    send_error = -EIO; back_pending = true;
    assert(msg_send_cmd(MSG_CMD_PREVIEW_EXIT) == -EIO);
    assert(!pending_preview && !back_pending && lawrec_control_preview_needs_close());
    reset(); allocation_failure = true;
    assert(msg_send_cmd(MSG_CMD_PREVIEW_ENTER) == -ENOMEM && !pending_preview);
    allocation_failure = false;

    rtsp_state = LAWREC_RTSP_STATE_LIVE; record_state = LAWREC_RECORD_STATE_RECORDING;
    lawrec_control_note_preview_uncertain();
    assert(rtsp_stops && record_stops && !lawrec_control_is_preview_active());
    reset();

    // Fresh processes must observe both preview and orphaned playback ownership.
    lawrec_control_note_preview_uncertain(); display_sync_generation = UINT32_MAX;
    ui_msg_proc(); assert(display_query_pending && sent.back().command == MSG_CMD_DISPLAY_QUERY);
    display_response(display_query_sequence, 1, 2, 0); // malformed flag
    assert(display_query_pending && lawrec_control_preview_needs_close());
    display_response(display_query_sequence, 0, 0, 0); // backend not ready
    assert(!display_query_pending && lawrec_control_playback_start("test.mp4") == -EBUSY);
    reset();
    for (int state=LAWREC_RTSP_STATE_STARTING;state<=LAWREC_RTSP_STATE_STOPPING;++state) {
        rtsp_state=state;
        assert(msg_send_cmd(MSG_CMD_DISPLAY_RECOVER)==-EBUSY && sent.empty());
    }
    rtsp_state=LAWREC_RTSP_STATE_IDLE;
    for (int state=LAWREC_RECORD_STATE_STARTING;state<=LAWREC_RECORD_STATE_STOPPING;++state) {
        record_state=state;
        assert(msg_send_cmd(MSG_CMD_DISPLAY_RECOVER)==-EBUSY && sent.empty());
    }
    record_state=LAWREC_RECORD_STATE_FAILED; media_mask=1u<<2;
    assert(msg_send_cmd(MSG_CMD_DISPLAY_RECOVER)==-EBUSY && sent.empty());
    media_mask=0; playback=true;
    assert(msg_send_cmd(MSG_CMD_DISPLAY_RECOVER)==-EBUSY && sent.empty());
    reset(); lawrec_control_note_preview_request(1); lawrec_control_note_preview_result(1);
    assert(msg_send_cmd(MSG_CMD_DISPLAY_RECOVER)==-EBUSY && sent.empty());
    reset(); lawrec_control_note_display_status(1,0,1);
    assert(msg_send_cmd(MSG_CMD_DISPLAY_RECOVER)==0 && pending_preview==MSG_CMD_DISPLAY_RECOVER);
    assert(lawrec_control_playback_start("test.mp4")==-EBUSY);
    response(MSG_CMD_DISPLAY_RECOVER_RESULT,preview_sequence,-EIO);
    assert(recovery_result==-EIO && lawrec_control_preview_needs_close());
    assert(msg_send_cmd(MSG_CMD_DISPLAY_RECOVER)==0);
    const int results=recovery_results;
    response(MSG_CMD_DISPLAY_RECOVER_RESULT,preview_sequence-1);
    assert(recovery_results==results && pending_preview==MSG_CMD_DISPLAY_RECOVER);
    response(MSG_CMD_DISPLAY_RECOVER_RESULT,preview_sequence);
    assert(recovery_result==0 && !lawrec_control_preview_needs_close());
    reset(); assert(!msg_send_cmd(MSG_CMD_DISPLAY_RECOVER));
    preview_deadline=std::chrono::steady_clock::now()-std::chrono::seconds(1);
    ui_msg_proc(); assert(recovery_result==-ETIMEDOUT && !pending_preview && lawrec_control_preview_needs_close());
    reset(); assert(!msg_send_cmd(MSG_CMD_DISPLAY_RECOVER));
    ipc_generation+=2; ui_msg_proc();
    assert(recovery_result==-ENOTCONN && lawrec_control_preview_needs_close());
    display_query_retry = std::chrono::steady_clock::time_point(); ui_msg_proc();
    display_response(display_query_sequence, 1, 0, 1); // failed unbind kept Bound
    assert(!lawrec_control_is_preview_active() && lawrec_control_preview_needs_close());
    assert(lawrec_control_playback_start("test.mp4") == -EBUSY);
    reset(); display_sync_generation = UINT32_MAX;
    ui_msg_proc(); display_response(display_query_sequence, 1, 0, 0, 1);
    lawrec_control_note_preview_result(0);
    assert(lawrec_control_preview_needs_close() && lawrec_control_playback_start("test.mp4") == -EBUSY);
    reset(); lawrec_control_note_preview_uncertain(); display_sync_generation = UINT32_MAX;
    ui_msg_proc(); display_response(display_query_sequence, 1, 0, 0);
    assert(!lawrec_control_preview_needs_close() && !lawrec_control_playback_start("test.mp4"));
    reset(); lawrec_control_note_preview_uncertain(); display_sync_generation = UINT32_MAX;
    ui_msg_proc(); const uint32_t stale_query = display_query_sequence;
    assert(!msg_send_cmd(MSG_CMD_PREVIEW_ENTER));
    display_response(stale_query, 1, 0, 0);
    assert(pending_preview == MSG_CMD_PREVIEW_ENTER && lawrec_control_preview_needs_close());
    reset(); ipc_generation += 2; ui_msg_proc();
    assert(display_query_pending && lawrec_control_playback_start("test.mp4") == -EBUSY);
    display_query_deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    ui_msg_proc();
    assert(!display_query_pending && lawrec_control_playback_start("test.mp4") == -EBUSY);
    puts("UI preview IPC: early Back, dual-consumer drain, failures, full-queue timeout, stale replies and reconnect passed (mock transport).");
    puts("Display sync: startup reservation, readiness retry, coherent busy flags, orphan playback and superseded query passed (mock transport).");
    puts("Display recovery: live/poisoned consumer guards, pending reservation, SDK errors, stale result, timeout and disconnect passed (mock transport).");
}
