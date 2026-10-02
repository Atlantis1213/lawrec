#include "lawrec_time.h"
#include "../little/src/control/include/lawrec_control.h"
#include "../little/src/record/include/lawrec_record_entry.h"
#include "../little/src/rtsp/include/lawrec_rtsp_entry.h"
#include "../little/src/playback/lawrec_playback.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <ctime>

static int rtsp_state, record_state;
static std::atomic<int> playback{0};
static int calls, set_error, read_error;
static struct timespec realtime{0, 0};
static std::mutex gate;
static std::condition_variable cv;
static bool hold_set, entered_set, release_set;

// Linker wrappers intercept only this test executable: no system clock is changed.
extern "C" int __real_clock_gettime(clockid_t, struct timespec *);
extern "C" int __wrap_clock_gettime(clockid_t id, struct timespec *value)
{
    if (id != CLOCK_REALTIME) return __real_clock_gettime(id, value);
    if (read_error) { errno = read_error; return -1; }
    *value = realtime;
    return 0;
}
extern "C" int __wrap_clock_settime(clockid_t id, const struct timespec *value)
{
    assert(id == CLOCK_REALTIME);
    ++calls;
    if (hold_set) {
        std::unique_lock<std::mutex> lock(gate);
        entered_set = true;
        cv.notify_all();
        cv.wait(lock, [] { return release_set; });
    }
    if (set_error) { errno = set_error; return -1; }
    realtime = *value;
    return 0;
}
extern "C" int lawrec_rtsp_get_state(void) { return rtsp_state; }
extern "C" int lawrec_record_get_state(void) { return record_state; }
extern "C" int lawrec_rtsp_stop_async(void) { rtsp_state = LAWREC_RTSP_STATE_IDLE; return 0; }
extern "C" int lawrec_record_stop_async(void) { record_state = LAWREC_RECORD_STATE_IDLE; return 0; }
extern "C" int lawrec_playback_active(void) { return playback; }
extern "C" int lawrec_playback_start(const char *) { playback = 1; return 0; }

int main()
{
    int64_t value;
    assert(lawrec_time_parse_utc(nullptr, &value) == -EINVAL);
    assert(lawrec_time_parse_utc("2026-01-01 00:00:00", nullptr) == -EINVAL);
    const char *bad_format[] = {"", "2026-1-01 00:00:00", "2026/01/01 00:00:00",
        "2026-01-01T00:00:00", "2026-01-01 00:00:00Z", "2026-01-01 00:00:0x"};
    for (const char *text : bad_format) assert(lawrec_time_parse_utc(text, &value) == -EINVAL);
    const char *bad_range[] = {"2019-12-31 00:00:00", "2100-01-01 00:00:00",
        "2026-00-01 00:00:00", "2026-13-01 00:00:00", "2026-01-00 00:00:00",
        "2026-04-31 00:00:00", "2026-02-29 00:00:00", "2026-01-01 24:00:00",
        "2026-01-01 00:60:00", "2026-01-01 00:00:60"};
    for (const char *text : bad_range) assert(lawrec_time_parse_utc(text, &value) == -ERANGE);
    assert(lawrec_time_parse_utc("2024-02-29 12:34:56", &value) == 0);
    assert(value == 1709210096);
    setenv("TZ", "UTC-8", 1); tzset();
    int64_t other;
    assert(lawrec_time_parse_utc("2024-02-29 12:34:56", &other) == 0 && other == value);
    assert(lawrec_time_parse_utc("2099-12-31 23:59:59", &other) == 0 && other == 4102444799LL);

    lawrec_control_set_log_path("/dev/null");
    const char *target = "2024-02-29 12:34:56";
    assert(lawrec_control_set_time_utc("bad") == -EINVAL && calls == 0);
    for (int state = LAWREC_RTSP_STATE_STARTING; state <= LAWREC_RTSP_STATE_STOPPING; ++state) {
        rtsp_state = state;
        assert(lawrec_control_set_time_utc(target) == -EBUSY && calls == 0);
    }
    rtsp_state = LAWREC_RTSP_STATE_IDLE;
    for (int state = LAWREC_RECORD_STATE_STARTING; state <= LAWREC_RECORD_STATE_STOPPING; ++state) {
        record_state = state;
        assert(lawrec_control_set_time_utc(target) == -EBUSY && calls == 0);
    }
    record_state = LAWREC_RECORD_STATE_IDLE;
    playback = 1;
    assert(lawrec_control_set_time_utc(target) == -EBUSY && calls == 0);
    playback = 0;
    set_error = EPERM;
    assert(lawrec_control_set_time_utc(target) == -EPERM && realtime.tv_sec == 0);
    set_error = 0;
    rtsp_state = LAWREC_RTSP_STATE_FAILED;
    record_state = LAWREC_RECORD_STATE_FAILED;
    auto before = std::chrono::steady_clock::now();
    assert(lawrec_control_set_time_utc(target) == 0);
    assert(std::chrono::steady_clock::now() >= before);
    assert(realtime.tv_sec == value && realtime.tv_nsec == 0);
    char text[32];
    assert(lawrec_time_current_utc(text, sizeof(text)) == 0 && !strcmp(text, target));
    assert(lawrec_time_current_utc(nullptr, sizeof(text)) == -EINVAL);
    assert(lawrec_time_current_utc(text, 19) == -EINVAL);
    read_error = EIO;
    assert(lawrec_time_current_utc(text, sizeof(text)) == -EIO);
    read_error = 0;

    // Keep the set operation in flight while a competing control start arrives.
    hold_set = true;
    auto update = std::async(std::launch::async, [=] { return lawrec_control_set_time_utc(target); });
    {
        std::unique_lock<std::mutex> lock(gate);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [] { return entered_set; }));
    }
    std::promise<void> attempting;
    auto attempted = attempting.get_future();
    auto start = std::async(std::launch::async, [&] {
        attempting.set_value();
        return lawrec_control_playback_start("test.mp4");
    });
    attempted.wait();
    assert(start.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    assert(playback == 0);
    {
        std::lock_guard<std::mutex> lock(gate);
        release_set = true;
    }
    cv.notify_all();
    assert(update.get() == 0 && start.get() == 0);
    assert(lawrec_control_set_time_utc(target) == -EBUSY);
    puts("time: UTC/date validation, clock errors, media guards and serialized start passed; real clock untouched");
}
