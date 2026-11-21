#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct Particle {
    uint32_t id;
    float x;
    float y;
    float vx;
    float vy;
    uint32_t color;
};

int main() {
    std::string status = "CALIBRATING";
    std::vector<Particle> particles;
    for (uint32_t i = 0; i < 16; ++i) {
        particles.push_back(Particle{
            i,
            static_cast<float>(i) * 1.5f,
            static_cast<float>(i * i) * 0.25f,
            0.5f + static_cast<float>(i) * 0.1f,
            -0.25f * static_cast<float>(i % 4),
            0xFF572200u | (i * 15u),
        });
    }

    std::unordered_map<std::string, int> counters{
        {"frames", 0},
        {"collisions", 3},
        {"alerts", 1},
    };

    auto primary = std::make_unique<Particle>(particles[0]);
    auto shared_probe = std::make_shared<std::string>("PROBE_A");
    auto shared_alias = shared_probe;
    std::optional<double> drift = 0.0042;

    for (int step = 0; step < 5; ++step) {
        status = (step < 3) ? "SAMPLING" : "LOCKED";
        for (auto& p : particles) {
            p.x += p.vx;
            p.y += p.vy;
        }
        primary->x = particles[step].x;
        counters["frames"] = step + 1;
        drift = *drift + 0.0015;
        std::printf("step=%d status=%s p0=(%.2f, %.2f) refcount=%ld\n",
                    step, status.c_str(), particles[0].x, particles[0].y,
                    shared_probe.use_count());
        std::fflush(stdout);
    }

    (void)shared_alias;
    return 0;
}
