#pragma once
#include "live_source.h"
#include <ServerMediaSession.hh>
#include <functional>

namespace demo {
ServerMediaSession *create_stream(UsageEnvironment &env, Feed &feed, MediaClock &clock,
    LiveDelivery &delivery, const std::vector<uint8_t> &sps, const std::vector<uint8_t> &pps,
    std::function<void()> request_idr);
}
