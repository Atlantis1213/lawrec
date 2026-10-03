#include "protocol.h"
#include <cassert>
#include <cstdio>
int main() {
    demo::Request request, decoded;
    assert(demo::decode_request(&request, sizeof(request), decoded));
    assert(!demo::decode_request(nullptr, sizeof(request), decoded));
    assert(!demo::decode_request(&request, sizeof(request) - 1, decoded));
    request.protocol_version++;
    assert(!demo::decode_request(&request, sizeof(request), decoded));
    request = {}; request.command = 5;
    assert(!demo::decode_request(&request, sizeof(request), decoded));
    request.command = 1; request.value = 2;
    assert(!demo::decode_request(&request, sizeof(request), decoded));
    request.value = 1;
    assert(demo::decode_request(&request, sizeof(request), decoded));
    demo::Status status; status.id = 9;
    assert(demo::valid_status(status, 9));
    assert(!demo::valid_status(status, 8));
    status.flags = 16;
    assert(!demo::valid_status(status, 9));
    std::puts("protocol: length/version/command/value/id/flags passed");
}
