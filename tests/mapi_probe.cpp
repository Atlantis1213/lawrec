// Connect/disconnect only: no media_init, camera, VB, encoder or audio calls.
#include "k_ipcmsg.h"
#include <cstdio>
#include <unistd.h>
namespace {
void ignore(int, k_ipcmsg_message_t *) {}
}
int main() {
    k_ipcmsg_connect_t attrs{1, 1, 1};
    int ret = kd_ipcmsg_add_service("kd_mapi_msg", &attrs);
    int handle = -1;
    if (!ret) ret = kd_ipcmsg_connect(&handle, "kd_mapi_msg", ignore);
    std::printf("MAPI transport probe connect=%d handle=%d (no hardware init)\n", ret, handle);
    if (!ret) {
        usleep(200000);
        ret = kd_ipcmsg_disconnect(handle);
    }
    kd_ipcmsg_del_service("kd_mapi_msg");
    std::printf("MAPI transport probe disconnect=%d\n", ret);
    return ret ? 1 : 0;
}
