// Standalone test for the background music decoder: decodes the real MP3s (data\music) through Media
// Foundation and checks the PCM that the mixer would receive.
// Usage: test_music_player.exe [music_dir]   (default: data\music next to the project root)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "audio/music_player.h"

using namespace zb::audio;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failed;
}

// Pulls `frames` frames like the mixer does, in real time, and returns the peak absolute sample.
int pull(int frames, int* nonzero = nullptr) {
    constexpr int kBlock = 1024;
    std::vector<int32_t> acc(kBlock * 2);
    int peak = 0, nz = 0;
    for (int done = 0; done < frames; done += kBlock) {
        std::fill(acc.begin(), acc.end(), 0);
        music::mix_into(acc.data(), kBlock);
        for (int32_t v : acc) {
            peak = std::max(peak, std::abs(v));
            nz += v != 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 * kBlock / music::kRate));
    }
    if (nonzero) *nonzero = nz;
    return peak;
}

}  // namespace

int main(int argc, char** argv) {
    music::set_music_dir(argc > 1 ? argv[1] : "data\\music");
    std::printf("[music] decoder tests\n");

    check(!music::play("no_such_track", true), "unknown track name is rejected");

    check(music::play("home", true), "play('home') accepted");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the decoder fill the ring
    int nz = 0;
    int peak = pull(music::kRate / 2, &nz);
    check(nz > 1000 && peak > 100, "decoded PCM is not silence");

    music::set_volume(0.0f);
    check(pull(music::kRate / 10) == 0, "volume 0 silences the output");
    music::set_volume(1.0f);

    music::pause();
    check(pull(music::kRate / 10) == 0, "pause outputs nothing");
    music::resume();
    peak = pull(music::kRate / 4);
    check(peak > 100, "resume outputs audio again");

    music::restart();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(pull(music::kRate / 4) > 100, "restart keeps decoding");

    music::set_suspended(true);
    check(pull(music::kRate / 10) == 0, "focus loss (suspended) silences the output");
    music::set_suspended(false);

    music::stop();
    check(pull(music::kRate / 10) == 0 && !music::is_playing(), "stop releases the player");

    check(music::play("credits", false), "play('credits', loop=false) accepted");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    peak = pull(music::kRate * 3);  // the track may open with a quiet intro
    std::printf("  (credits peak over the first 3 s: %d)\n", peak);
    check(peak > 100, "a second track plays after the first was stopped");
    music::stop();

    std::printf(g_failed ? "MUSIC TESTS FAILED (%d)\n" : "ALL MUSIC TESTS PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
