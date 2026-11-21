#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

struct DeviceConfig {
    uint64_t id;
    uint64_t bus_addr;
    uint64_t calibration_offset;
    int32_t enabled;
};

static int read_device_enabled(DeviceConfig* cfg) {
    // Intentional null+offset dereference for Crash Autopsy demo
    return cfg->enabled;
}

static int boot_sequence(bool trigger_assert) {
    if (trigger_assert) {
        int rail_mv = 0;
        assert(rail_mv > 3000 && "Power rail undervoltage");
    }
    DeviceConfig* missing_cfg = nullptr;
    return read_device_enabled(missing_cfg);
}

int main(int argc, char** argv) {
    bool do_assert = (argc > 1 && std::strcmp(argv[1], "--assert") == 0);
    std::printf("starting crash_autopsy (mode=%s)\n", do_assert ? "assert" : "segfault");
    std::fflush(stdout);
    int res = boot_sequence(do_assert);
    std::printf("result=%d\n", res);
    return 0;
}
