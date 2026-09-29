#pragma once

#include <atomic>
#include <fstream>
#include <string>

#include "../../util.h"

class LawrecRtspCompat {
  public:
    LawrecRtspCompat(uint32_t port, const char *stream_name);

    int StartRequested();
    int StopRequested();
    void FillStatus(lawrec_rtsp_status_t *status) const;
    bool Requested() const;

  private:
    std::atomic<bool> requested_;
    uint32_t port_;
    std::string stream_name_;
};
