#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

struct SensorFrame {
    uint32_t seq;
    float voltage;
    float temperature;
    uint16_t flags;
    const char* label;
};

static float compute_filtered(float prev, float sample, float alpha) {
    float delta = sample - prev;
    float next = prev + alpha * delta;
    return next;
}

int main() {
    SensorFrame frame{1, 3.30f, 24.5f, 0x00A1, "CH1_RAIL"};
    SensorFrame* frame_ptr = &frame;
    SensorFrame* null_ptr = nullptr;

    std::vector<float> waveform;
    waveform.reserve(64);

    float filtered = 0.0f;
    int checksum = 0;

    for (int i = 0; i < 64; ++i) {
        float t = static_cast<float>(i) * 0.25f;
        float sample = std::sin(t) * 2.5f + std::cos(t * 3.0f) * 0.6f;
        filtered = compute_filtered(filtered, sample, 0.35f);
        waveform.push_back(filtered);

        frame.seq = static_cast<uint32_t>(i + 1);
        frame.voltage = 3.30f + filtered * 0.1f;
        frame.temperature = 24.5f + static_cast<float>(i) * 0.05f;
        checksum += static_cast<int>(filtered * 100.0f);

        if (i % 16 == 0) {
            std::printf("seq=%u v=%.3f temp=%.2f checksum=%d\n",
                        frame_ptr->seq, frame_ptr->voltage, frame_ptr->temperature, checksum);
            std::fflush(stdout);
        }
    }

    (void)null_ptr;
    std::printf("done: samples=%zu checksum=%d\n", waveform.size(), checksum);
    return 0;
}
