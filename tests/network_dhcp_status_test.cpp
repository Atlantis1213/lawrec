// Only the passive snapshot path is retained; no socket/IP/media operations.
#include "../little/src/common/lawrec_network.cpp"
#include <cassert>
#include <condition_variable>
#include <new>

static lawrec_dhcp_status lease;
static int calls = 0, failure = 0;
static bool throws = false, blocked = false, entered = false;
static std::mutex gate;
static std::condition_variable changed;
int lawrec_dhcp_get(lawrec_dhcp_status *s) {
    std::unique_lock<std::mutex> lock(gate);
    ++calls;
    entered = true; changed.notify_all();
    changed.wait(lock, [] { return !blocked; });
    if (throws) throw std::bad_alloc();
    *s = lease;
    return failure;
}

static void allow_poll() {
    std::lock_guard<std::mutex> lock(runtime.lock);
    runtime.last_lease_poll = {};
}
int main() {
    lawrec_network_get(nullptr); assert(!calls);
    lease.pid = 123; lease.bound = 1; lease.lease_seconds = lease.remaining_seconds = 60;
    lawrec_network_snapshot s;
    lawrec_network_get(&s);
    assert(calls == 1 && s.dhcp_bound && s.dhcp_pid == 123 && s.lease_remaining_seconds == 60);
    const unsigned first = s.generation;
    lease.remaining_seconds = 59;
    lawrec_network_get(&s);
    assert(calls == 1 && s.generation == first); // No per-LVGL-tick /proc scan.
    allow_poll(); lawrec_network_get(&s);
    assert(calls == 2 && s.lease_remaining_seconds == 59 && s.generation == first+1);
    lease.bound = 0; lease.remaining_seconds = 0; lease.error = -ETIMEDOUT;
    allow_poll(); lawrec_network_get(&s);
    assert(!s.dhcp_bound && s.dhcp_error == -ETIMEDOUT && s.dhcp_pid == 123);

    // A lease observation must not overwrite a concurrently-started operation.
    {
        std::lock_guard<std::mutex> lock(gate);
        blocked = true; entered = false;
    }
    allow_poll();
    std::thread reader([&] { lawrec_network_get(&s); });
    {
        std::unique_lock<std::mutex> lock(gate);
        assert(changed.wait_for(lock, std::chrono::seconds(2), [] { return entered; }));
    }
    {
        std::lock_guard<std::mutex> lock(runtime.lock);
        runtime.snapshot.busy = 1; runtime.snapshot.dhcp_pid = 456;
        ++runtime.snapshot.generation;
    }
    { std::lock_guard<std::mutex> lock(gate); blocked = false; changed.notify_all(); }
    reader.join();
    assert(s.busy && s.dhcp_pid == 456);
    const int before = calls;
    allow_poll(); lawrec_network_get(&s); assert(calls == before);
    { std::lock_guard<std::mutex> lock(runtime.lock); runtime.snapshot.busy = 0; }
    // Also reject a stale observation after an operation has already completed.
    {
        std::lock_guard<std::mutex> lock(gate);
        blocked = true; entered = false;
    }
    allow_poll();
    reader = std::thread([&] { lawrec_network_get(&s); });
    {
        std::unique_lock<std::mutex> lock(gate);
        assert(changed.wait_for(lock, std::chrono::seconds(2), [] { return entered; }));
    }
    {
        std::lock_guard<std::mutex> lock(runtime.lock);
        runtime.snapshot.dhcp_pid = 789;
        runtime.snapshot.scan_generation = 17;
        ++runtime.snapshot.generation;
    }
    { std::lock_guard<std::mutex> lock(gate); blocked = false; changed.notify_all(); }
    reader.join();
    assert(!s.busy && s.dhcp_pid == 789 && s.scan_generation == 17);
    lease.bound = 1; lease.error = 0; lease.remaining_seconds = 60;
    allow_poll(); lawrec_network_get(&s); assert(s.dhcp_bound && !s.dhcp_error);
    assert(s.scan_generation == 17);
    failure = -EPROTO;
    allow_poll(); lawrec_network_get(&s);
    assert(!s.dhcp_bound && !s.lease_remaining_seconds && !s.lease_seconds && s.dhcp_error == -EPROTO);
    failure = 0; throws = true;
    allow_poll(); lawrec_network_get(&s);
    assert(!s.dhcp_bound && !s.dhcp_pid && s.dhcp_error == -ENOMEM);
    throws = false;
    puts("network DHCP snapshot: passive countdown, throttling, errors, exceptions and busy/generation race passed");
}
