#pragma once
#ifdef __cplusplus
#include <atomic>
#include <vector>
#include <sys/types.h>
#include "mp4_format.h"

// Only file parsing runs in the helper. MAPI/display/VB remain in the UI worker.
class LawrecDemux {
public:
    explicit LawrecDemux(const std::atomic<bool> &cancel) : cancel_(cancel) {}
    ~LawrecDemux();
    LawrecDemux(const LawrecDemux &) = delete;
    LawrecDemux &operator=(const LawrecDemux &) = delete;
    int start(int file, k_mp4_file_info_s &info, k_mp4_track_info_s tracks[2]);
    int next(k_mp4_frame_data_s &frame);
    int close();
private:
    int exchange(unsigned operation, void *response);
    const std::atomic<bool> &cancel_;
    pid_t pid_ = -1;
    int socket_ = -1;
    bool reserved_ = false;
    std::vector<unsigned char> bytes_;
};
extern "C" {
#endif
/* Headless private entry; argc/argv are passed before UI and media init. */
int lawrec_demux_helper(int argc, char **argv);
#ifdef __cplusplus
}
#endif
