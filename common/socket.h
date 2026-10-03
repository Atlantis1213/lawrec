#pragma once
#include "protocol.h"
namespace demo {
int exchange(const char *path, const Request &request, Status &status, int timeout_ms = 1000);
}
