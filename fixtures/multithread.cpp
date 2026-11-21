#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

static std::mutex g_bus_mutex;
static int g_shared_counter = 0;
static std::atomic<bool> g_stop{false};

static void worker_loop(int worker_id) {
    int local_ops = 0;
    while (!g_stop.load()) {
        {
            std::lock_guard<std::mutex> lock(g_bus_mutex);
            g_shared_counter += (worker_id + 1);
            ++local_ops;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    std::printf("worker %d finished ops=%d\n", worker_id, local_ops);
    std::fflush(stdout);
}

int main() {
    std::vector<std::thread> workers;
    for (int i = 0; i < 3; ++i) {
        workers.emplace_back(worker_loop, i);
    }

    for (int tick = 0; tick < 8; ++tick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        int snapshot = 0;
        {
            std::lock_guard<std::mutex> lock(g_bus_mutex);
            snapshot = g_shared_counter;
        }
        std::printf("tick=%d shared_counter=%d\n", tick, snapshot);
        std::fflush(stdout);
    }

    g_stop.store(true);
    for (auto& t : workers) {
        t.join();
    }
    return 0;
}
