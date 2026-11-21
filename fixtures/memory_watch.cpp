#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

struct TelemetryPacket {
    uint32_t magic;
    uint16_t channel;
    uint16_t flags;
    float gain;
    double timestamp;
};

struct ListNode {
    int id;
    float weight;
    ListNode* next;
};

static void corrupt_gain_subtly(TelemetryPacket* pkt, int iter) {
    if (iter == 3) {
        pkt->gain = -999.25f;
        pkt->flags |= 0x8000u;
    }
}

int main() {
    auto* packet = new TelemetryPacket{0xCAFEBABEu, 4, 0x001F, 1.50f, 1024.125};

    // 32x32 RGBA procedural test image for the 2D Pixel Probe
    constexpr int W = 32;
    constexpr int H = 32;
    std::vector<uint8_t> framebuffer(W * H * 4, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            size_t idx = static_cast<size_t>((y * W + x) * 4);
            bool grid = (x % 8 == 0) || (y % 8 == 0);
            framebuffer[idx + 0] = grid ? 255 : static_cast<uint8_t>(x * 8);
            framebuffer[idx + 1] = grid ? 87 : static_cast<uint8_t>(y * 8);
            framebuffer[idx + 2] = grid ? 34 : static_cast<uint8_t>((x ^ y) * 6);
            framebuffer[idx + 3] = 255;
        }
    }
    uint8_t* pixels = framebuffer.data();

    // Linked list with cycle for Pointer Graph Probe
    ListNode n4{40, 4.0f, nullptr};
    ListNode n3{30, 3.0f, &n4};
    ListNode n2{20, 2.0f, &n3};
    ListNode n1{10, 1.0f, &n2};
    n4.next = &n2; // intentional cycle back to n2
    ListNode* head = &n1;

    for (int i = 0; i < 6; ++i) {
        packet->timestamp += 0.5;
        packet->channel = static_cast<uint16_t>(i);
        corrupt_gain_subtly(packet, i);
        std::printf("iter=%d gain=%.2f flags=0x%04x head=%p pixels=%p\n",
                    i, packet->gain, packet->flags,
                    static_cast<void*>(head), static_cast<void*>(pixels));
        std::fflush(stdout);
    }

    delete packet;
    return 0;
}
