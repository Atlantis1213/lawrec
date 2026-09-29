#include "lawrec_rtsp_compat.h"

#include <cstdio>
#include <cstring>

LawrecRtspCompat::LawrecRtspCompat(uint32_t port, const char *stream_name)
    : requested_(false),
      port_(port),
      stream_name_(stream_name != NULL ? stream_name : "lawrec")
{
}

int LawrecRtspCompat::StartRequested()
{
    requested_.store(true);
    printf("[lawrec] rtsp request accepted on big core compatibility path; "
           "small core service owns the RTSP server\n");
    return 0;
}

int LawrecRtspCompat::StopRequested()
{
    requested_.store(false);
    return 0;
}

void LawrecRtspCompat::FillStatus(lawrec_rtsp_status_t *status) const
{
    if (status == NULL)
        return;

    memset(status, 0, sizeof(*status));
    status->enabled = requested_.load() ? 1 : 0;
    status->port = port_;
    snprintf(status->stream_name, sizeof(status->stream_name), "%s",
             stream_name_.c_str());
}

bool LawrecRtspCompat::Requested() const
{
    return requested_.load();
}
