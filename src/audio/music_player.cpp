#include "audio/music_player.h"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/common.h"

namespace zb::audio::music {
namespace {

// Four seconds are plenty: the decoder stops as soon as the ring is half full.
constexpr size_t kRingHigh = static_cast<size_t>(kRate) * 2 * 2;

std::string g_dir = "data\\music";
void (*g_starter)() = nullptr;
std::atomic<float> g_volume{1.0f};
std::atomic<bool> g_suspended{false};

// ------------------------------------------------------------ Media Foundation decoder

template <class T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

class Decoder {
public:
    ~Decoder() { release(reader_); }

    bool open(const std::wstring& path) {
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader_))) return false;
        IMFMediaType* t = nullptr;
        if (FAILED(MFCreateMediaType(&t))) return false;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 4);
        const HRESULT hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t);
        release(t);
        return SUCCEEDED(hr);
    }

    // Appends the next block of decoded samples; false at the end of the stream (or on error).
    bool read(std::vector<int16_t>& out) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        if (FAILED(reader_->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample)))
            return false;
        if (sample) {
            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                    const size_t n = len / sizeof(int16_t);
                    const size_t at = out.size();
                    out.resize(at + n);
                    std::memcpy(out.data() + at, data, n * sizeof(int16_t));
                    buf->Unlock();
                }
                release(buf);
            }
            release(sample);
        }
        return !(flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR));
    }

    void rewind() {
        PROPVARIANT v;
        std::memset(&v, 0, sizeof v);
        v.vt = VT_I8;
        v.hVal.QuadPart = 0;
        reader_->SetCurrentPosition(GUID_NULL, v);
    }

private:
    IMFSourceReader* reader_ = nullptr;
};

void ensure_mf() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) ZLOGW("music: Media Foundation failed to start");
    });
}

// ------------------------------------------------------------ one playing track

struct Track {
    std::wstring path;
    bool loop = true;
    std::mutex m;
    std::condition_variable cv;
    std::deque<int16_t> ring;
    bool stop = false;
    bool restart = false;
    bool ended = false;  // decoder reached the end of a non-looping track
    bool audible = false;  // the mixer has pulled samples at least once
    std::atomic<bool> paused{false};
    std::thread worker;

    void run() {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ensure_mf();
        Decoder dec;
        if (!dec.open(path)) {
            ZLOGW("music: cannot decode %ls", path.c_str());
        } else {
            std::vector<int16_t> block;
            for (;;) {
                {
                    std::unique_lock lk(m);
                    cv.wait(lk, [&] { return stop || restart || (!ended && ring.size() < kRingHigh); });
                    if (stop) break;
                    if (restart) {
                        restart = false;
                        ended = false;
                        ring.clear();
                        lk.unlock();
                        dec.rewind();
                    }
                }
                block.clear();
                const bool more = dec.read(block);
                {
                    std::lock_guard lk(m);
                    ring.insert(ring.end(), block.begin(), block.end());
                }
                if (!more) {
                    if (loop) {
                        dec.rewind();
                    } else {
                        std::lock_guard lk(m);
                        ended = true;
                    }
                }
            }
        }
        if (com) CoUninitialize();
    }
};

std::mutex g_mu;
std::shared_ptr<Track> g_cur;

std::shared_ptr<Track> current() {
    std::lock_guard lk(g_mu);
    return g_cur;
}

void halt(std::shared_ptr<Track> t) {
    if (!t) return;
    {
        std::lock_guard lk(t->m);
        t->stop = true;
    }
    t->cv.notify_all();
    if (t->worker.joinable()) t->worker.join();
}

const char* file_for(const std::string& track) {
    static const char* const known[] = {"gardens", "canyons", "avalonia", "home", "homexmas", "credits"};
    for (const char* k : known)
        if (track == k) return k;
    return nullptr;
}

}  // namespace

void set_music_dir(const std::string& dir) {
    if (!dir.empty()) g_dir = dir;
}

void set_output_starter(void (*fn)()) { g_starter = fn; }

bool play(const std::string& track, bool loop) {
    stop();
    const char* name = file_for(track);
    if (!name) {
        ZLOGW("music: unknown track '%s'", track.c_str());
        return false;
    }
    const std::string path = g_dir + "\\" + name + ".mp3";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ZLOGW("music: %s not found (import them with tools\\import_game_data.ps1)", path.c_str());
        return true;  // the original reports success for every known track
    }
    if (g_starter) g_starter();
    auto t = std::make_shared<Track>();
    t->path.assign(path.begin(), path.end());  // ASCII paths only: the folder is configured by the user/installer
    t->loop = loop;
    {
        std::lock_guard lk(g_mu);
        g_cur = t;
    }
    t->worker = std::thread([t] { t->run(); });
    ZLOGD("music: playing %s (loop=%d)", path.c_str(), loop);
    return true;
}

void pause() {
    if (auto t = current()) t->paused = true;
}

void resume() {
    if (auto t = current()) t->paused = false;
}

void restart() {
    if (auto t = current()) {
        {
            std::lock_guard lk(t->m);
            t->restart = true;
        }
        t->paused = false;
        t->cv.notify_all();
    }
}

void stop() {
    std::shared_ptr<Track> t;
    {
        std::lock_guard lk(g_mu);
        t.swap(g_cur);
    }
    halt(std::move(t));
}

void set_volume(float v) { g_volume = std::clamp(v, 0.0f, 1.0f); }

void set_suspended(bool suspended) { g_suspended = suspended; }

bool is_playing() {
    auto t = current();
    return t && !t->paused && !g_suspended;
}

void mix_into(int32_t* acc, int frames) {
    auto t = current();
    if (!t || t->paused || g_suspended) return;
    const float g = g_volume;
    bool wake = false;
    {
        std::lock_guard lk(t->m);
        const size_t want = std::min(static_cast<size_t>(frames) * 2, t->ring.size());
        if (want > 0 && !t->audible) {
            t->audible = true;
            ZLOGD("music: first audio block mixed (%ls)", t->path.c_str());
        }
        for (size_t i = 0; i < want; ++i) acc[i] += static_cast<int32_t>(static_cast<float>(t->ring[i]) * g);
        t->ring.erase(t->ring.begin(), t->ring.begin() + static_cast<std::ptrdiff_t>(want));
        wake = t->ring.size() < kRingHigh;
    }
    if (wake) t->cv.notify_one();
}

}  // namespace zb::audio::music
