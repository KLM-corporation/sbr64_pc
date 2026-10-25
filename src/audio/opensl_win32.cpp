// OpenSL ES for the guest, implemented on the host with WinMM (waveOut).
//
// Guest interface handles are 8-byte structs in guest memory: [0] guest vtable, [4] host object id.
// Only what the game needs is implemented: an engine, an output mix and PCM buffer-queue audio players
// (play/stop/pause, volume, playback rate, queue callbacks). Everything else answers
// SL_RESULT_FEATURE_UNSUPPORTED. All players are mixed on one thread into a 44.1 kHz stereo stream.
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "android/bionic_hle.h"
#include "android/runner.h"
#include "audio/music_player.h"
#include "core/cpu.h"
#include "core/mem_win32.h"
#include "core/thunk.h"

namespace zb::android {
namespace {

constexpr u32 SL_OK = 0, SL_PRECONDITIONS = 1, SL_PARAM_INVALID = 2, SL_UNSUPPORTED = 12;
constexpr u32 SL_STOPPED = 1, SL_PAUSED = 2, SL_PLAYING = 3;
constexpr u32 SL_LOC_URI = 3, SL_LOC_BUFFERQUEUE = 6, SL_LOC_ASB = 0x800007BD, SL_FMT_PCM = 2;
constexpr int kMixRate = 44100;

enum Itf { I_OBJECT, I_ENGINE, I_PLAY, I_VOLUME, I_BQ, I_EFFECT, I_RATE, I_CAPS, I_COUNT };
enum Kind { K_ENGINE, K_OUTMIX, K_PLAYER };

struct Buf {
    gaddr data;
    u32 size;
    u32 pos;
};

struct Obj {
    Kind kind = K_ENGINE;
    gaddr itf[I_COUNT] = {};
    bool realized = false;
    // player state
    int channels = 2, bits = 16;
    double src_rate = 44100;
    std::deque<Buf> queue;
    u32 play_state = SL_STOPPED;
    i16 volume_mb = 0;
    bool mute = false;
    u32 rate_permille = 1000;
    double frac = 0;  // fractional source position in frames within the head buffer
    gaddr bq_cb = 0, bq_ctx = 0;
    gaddr play_cb = 0, play_ctx = 0;
    u32 events_mask = 0;
    u32 play_index = 0;
    gaddr obj_cb = 0, obj_ctx = 0;
};

std::mutex g_mutex;
std::unordered_map<u32, std::unique_ptr<Obj>> g_objs;
u32 g_next_id = 1;
std::vector<gaddr> g_vtables(I_COUNT, 0);
std::deque<gaddr> g_graveyard;
std::vector<std::array<u8, 16>> g_iid_bytes(I_COUNT + 1);  // distinct GUIDs; index I_COUNT = android simple bq

Obj* find_obj(u32 id) {
    auto it = g_objs.find(id);
    return it == g_objs.end() ? nullptr : it->second.get();
}

gaddr make_itf(Itf type, u32 id) {
    gaddr p = mem::gcalloc(1, 8);
    wr<u32>(p, g_vtables[type]);
    wr<u32>(p + 4, id);
    return p;
}

u32 new_object(Kind kind, gaddr* out_itf) {
    std::lock_guard lk(g_mutex);
    u32 id = g_next_id++;
    auto o = std::make_unique<Obj>();
    o->kind = kind;
    o->itf[I_OBJECT] = make_itf(I_OBJECT, id);
    *out_itf = o->itf[I_OBJECT];
    g_objs[id] = std::move(o);
    return id;
}

// ------------------------------------------------------------ mixer

HWAVEOUT g_wave = nullptr;
std::once_flag g_mixer_once;
constexpr int kBuffers = 4, kFramesPerBuffer = 1024;
std::vector<int16_t> g_mix_pcm[kBuffers];
WAVEHDR g_hdr[kBuffers];
HANDLE g_wave_event = nullptr;

struct PendingCb {
    u32 id;
    gaddr fn, a, b, c;
    int nargs;
};

float gain_of(const Obj& o) {
    if (o.mute) return 0.f;
    return std::pow(10.f, static_cast<float>(o.volume_mb) / 2000.f);
}

// Mixes `frames` stereo frames into out (int16). Collects the callbacks to run once the lock is released.
void mix_block(int16_t* out, int frames, std::vector<PendingCb>& cbs) {
    std::vector<int32_t> acc(static_cast<size_t>(frames) * 2, 0);
    std::lock_guard lk(g_mutex);
    for (auto& [id, up] : g_objs) {
        Obj& o = *up;
        if (o.kind != K_PLAYER || o.play_state != SL_PLAYING) continue;
        const double step = o.src_rate / kMixRate * (o.rate_permille / 1000.0);
        const float g = gain_of(o);
        const int bytes_per_frame = std::max(1, o.channels * (o.bits / 8));
        for (int f = 0; f < frames; ++f) {
            if (o.queue.empty()) break;
            Buf& b = o.queue.front();
            u32 frame_index = static_cast<u32>(o.frac);
            u32 total_frames = b.data ? (b.size / static_cast<u32>(bytes_per_frame)) : 0;
            if (frame_index >= total_frames) {
                // buffer finished
                o.frac = (total_frames > 0 && o.frac >= total_frames) ? (o.frac - total_frames) : 0.0;
                o.queue.pop_front();
                if (o.queue.empty()) o.frac = 0.0;
                o.play_index++;
                if (o.bq_cb) cbs.push_back({id, o.bq_cb, o.itf[I_BQ], o.bq_ctx, 0, 3});
                if (o.queue.empty() && (o.events_mask & 1) && o.play_cb)
                    cbs.push_back({id, o.play_cb, o.itf[I_PLAY], o.play_ctx, 1, 4});
                --f;
                continue;
            }
            const u8* p = hp<const u8>(b.data) + static_cast<size_t>(frame_index) * bytes_per_frame;
            float l, r;
            if (o.bits == 16) {
                const int16_t* s = reinterpret_cast<const int16_t*>(p);
                l = s[0];
                r = o.channels > 1 ? s[1] : s[0];
            } else {
                l = (static_cast<int>(p[0]) - 128) * 256.f;
                r = o.channels > 1 ? (static_cast<int>(p[1]) - 128) * 256.f : l;
            }
            acc[static_cast<size_t>(f) * 2] += static_cast<int32_t>(l * g);
            acc[static_cast<size_t>(f) * 2 + 1] += static_cast<int32_t>(r * g);
            o.frac += step;
        }
    }
    audio::music::mix_into(acc.data(), frames);
    for (size_t i = 0; i < acc.size(); ++i) out[i] = static_cast<int16_t>(std::clamp(acc[i], -32768, 32767));
}

void run_pending_cbs(const std::vector<PendingCb>& cbs) {
    for (const PendingCb& c : cbs) {
        {
            std::lock_guard lk(g_mutex);
            Obj* o = find_obj(c.id);
            if (!o || o->play_state != SL_PLAYING) continue;
        }
        if (c.nargs == 3) cpu::call(c.fn, cpu::Args().u32v(c.a).u32v(c.b));
        else cpu::call(c.fn, cpu::Args().u32v(c.a).u32v(c.b).u32v(c.c));
    }
}

void mixer_thread() {
    // Prime and keep the ring of buffers full; the guest callbacks run here, outside the objects lock.
    for (;;) {
        if (!g_wave) {
            std::vector<PendingCb> cbs;
            mix_block(g_mix_pcm[0].data(), kFramesPerBuffer, cbs);
            run_pending_cbs(cbs);
            Sleep(20);
            continue;
        }
        for (int i = 0; i < kBuffers; ++i) {
            if (g_hdr[i].dwFlags & WHDR_INQUEUE) continue;  // still playing
            std::vector<PendingCb> cbs;
            mix_block(g_mix_pcm[i].data(), kFramesPerBuffer, cbs);
            g_hdr[i].dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
            waveOutWrite(g_wave, &g_hdr[i], sizeof(WAVEHDR));
            run_pending_cbs(cbs);
        }
        WaitForSingleObject(g_wave_event, 50);
    }
}

void ensure_mixer() {
    std::call_once(g_mixer_once, [] {
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = kMixRate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = kMixRate * 4;
        g_wave_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (waveOutOpen(&g_wave, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(g_wave_event), 0, CALLBACK_EVENT) !=
            MMSYSERR_NOERROR) {
            ZLOGW("OpenSL: no audio output device, running silent mixer");
            g_wave = nullptr;
            g_mix_pcm[0].assign(static_cast<size_t>(kFramesPerBuffer) * 2, 0);
            std::thread(mixer_thread).detach();
            return;
        }
        for (int i = 0; i < kBuffers; ++i) {
            g_mix_pcm[i].assign(static_cast<size_t>(kFramesPerBuffer) * 2, 0);
            std::memset(&g_hdr[i], 0, sizeof(WAVEHDR));
            g_hdr[i].lpData = reinterpret_cast<LPSTR>(g_mix_pcm[i].data());
            g_hdr[i].dwBufferLength = static_cast<DWORD>(g_mix_pcm[i].size() * 2);
            waveOutPrepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
            g_hdr[i].dwFlags |= WHDR_DONE;  // free: the mixer thread fills and queues it
        }
        std::thread(mixer_thread).detach();
    });
}

// ------------------------------------------------------------ helpers

u32 self_id(gaddr s) { return s ? rd<u32>(s + 4) : 0; }

#define OBJ(var)                                   \
    std::lock_guard lk(g_mutex);                   \
    Obj* var = find_obj(self_id(s));               \
    if (!var) return SL_PRECONDITIONS

u32 u_Unsupported() { return SL_UNSUPPORTED; }
u32 u_Success() { return SL_OK; }

// ------------------------------------------------------------ SLObjectItf

u32 o_Realize(gaddr s, u32) {
    OBJ(o);
    o->realized = true;
    return SL_OK;
}
u32 o_Resume(gaddr s, u32) { return SL_OK; }
u32 o_GetState(gaddr s, gp<u32> st) {
    OBJ(o);
    if (st.a) *st.get() = o->realized ? 2 : 1;  // SL_OBJECT_STATE_REALIZED / UNREALIZED
    return SL_OK;
}

int itf_from_iid(gaddr iid) {
    if (!iid) return -1;
    for (int i = 0; i <= I_COUNT; ++i)
        if (std::memcmp(hp(iid), g_iid_bytes[static_cast<size_t>(i)].data(), 16) == 0) return i;
    return -1;
}

u32 o_GetInterface(gaddr s, gaddr iid, gp<u32> out) {
    OBJ(o);
    int t = itf_from_iid(iid);
    if (t == I_COUNT) t = I_BQ;  // SL_IID_ANDROIDSIMPLEBUFFERQUEUE shares the buffer queue layout
    if (t < 0 || t == I_OBJECT) return SL_PARAM_INVALID;
    bool ok = (t == I_ENGINE && o->kind == K_ENGINE) || (t == I_CAPS && o->kind == K_ENGINE) ||
              (o->kind == K_PLAYER && (t == I_PLAY || t == I_VOLUME || t == I_BQ || t == I_EFFECT || t == I_RATE)) ||
              (o->kind == K_OUTMIX && (t == I_VOLUME || t == I_EFFECT));
    if (!ok) {
        ZLOGD("OpenSL GetInterface: interface %d not supported on object kind %d", t, o->kind);
        return SL_UNSUPPORTED;
    }
    if (!o->itf[t]) o->itf[t] = make_itf(static_cast<Itf>(t), self_id(s));
    *out.get() = o->itf[t];
    return SL_OK;
}
u32 o_RegisterCallback(gaddr s, gaddr fn, gaddr ctx) {
    OBJ(o);
    o->obj_cb = fn;
    o->obj_ctx = ctx;
    return SL_OK;
}
void o_AbortAsyncOperation(gaddr) {}
void o_Destroy(gaddr s) {
    std::lock_guard lk(g_mutex);
    auto it = g_objs.find(self_id(s));
    if (it == g_objs.end()) return;
    for (gaddr p : it->second->itf) {
        if (!p) continue;
        wr<u32>(p + 4, 0);  // dead: later calls through it fail cleanly
        g_graveyard.push_back(p);
    }
    g_objs.erase(it);
    while (g_graveyard.size() > 4096) {
        mem::gfree(g_graveyard.front());
        g_graveyard.pop_front();
    }
}
u32 o_SetPriority(gaddr, i32, u32) { return SL_OK; }
u32 o_GetPriority(gaddr, gp<i32> p, gp<u32> pre) {
    if (p.a) *p.get() = 0;
    if (pre.a) *pre.get() = 0;
    return SL_OK;
}
u32 o_SetLossOfControlInterfaces(gaddr, i16, gaddr, u32) { return SL_OK; }

// ------------------------------------------------------------ SLEngineItf

u32 e_CreateOutputMix(gaddr, gp<u32> out, u32, gaddr, gaddr) {
    gaddr itf;
    new_object(K_OUTMIX, &itf);
    *out.get() = itf;
    return SL_OK;
}

u32 e_CreateAudioPlayer(gaddr, gp<u32> out, gaddr src, gaddr snk, u32, gaddr, gaddr) {
    if (!src || !snk) return SL_PARAM_INVALID;
    gaddr loc = rd<u32>(src), fmt = rd<u32>(src + 4);
    if (!loc) return SL_PARAM_INVALID;
    u32 loc_type = rd<u32>(loc);
    if (loc_type != SL_LOC_BUFFERQUEUE && loc_type != SL_LOC_ASB) {
        ZLOGW("OpenSL CreateAudioPlayer: unsupported data locator 0x%x (only PCM buffer queues are implemented)", loc_type);
        return SL_UNSUPPORTED;
    }
    if (!fmt || rd<u32>(fmt) != SL_FMT_PCM) {
        ZLOGW("OpenSL CreateAudioPlayer: unsupported data format");
        return SL_UNSUPPORTED;
    }
    ensure_mixer();
    gaddr itf;
    u32 id = new_object(K_PLAYER, &itf);
    {
        std::lock_guard lk(g_mutex);
        Obj* o = find_obj(id);
        o->channels = static_cast<int>(rd<u32>(fmt + 4));
        o->src_rate = rd<u32>(fmt + 8) / 1000.0;
        o->bits = static_cast<int>(rd<u32>(fmt + 12));
        if (o->channels < 1 || o->channels > 2 || (o->bits != 8 && o->bits != 16) || o->src_rate <= 0) {
            ZLOGW("OpenSL CreateAudioPlayer: unsupported PCM format (%d ch, %d bit, %.0f Hz)", o->channels, o->bits, o->src_rate);
        }
    }
    *out.get() = itf;
    return SL_OK;
}

// ------------------------------------------------------------ SLPlayItf

u32 p_SetPlayState(gaddr s, u32 st) {
    OBJ(o);
    if (st == SL_STOPPED) o->frac = 0;
    o->play_state = st;
    return SL_OK;
}
u32 p_GetPlayState(gaddr s, gp<u32> st) {
    OBJ(o);
    if (st.a) *st.get() = o->play_state;
    return SL_OK;
}
u32 p_GetDuration(gaddr s, gp<u32> ms) {
    OBJ(o);
    if (ms.a) {
        if (!o->queue.empty() && o->src_rate > 0) {
            const int bytes_per_frame = std::max(1, o->channels * (o->bits / 8));
            u32 frames = o->queue.front().size / static_cast<u32>(bytes_per_frame);
            *ms.get() = static_cast<u32>(frames * 1000.0 / o->src_rate);
        } else {
            *ms.get() = 0xFFFFFFFFu;
        }
    }
    return SL_OK;
}
u32 p_RegisterCallback(gaddr s, gaddr fn, gaddr ctx) {
    OBJ(o);
    o->play_cb = fn;
    o->play_ctx = ctx;
    return SL_OK;
}
u32 p_SetCallbackEventsMask(gaddr s, u32 m) {
    OBJ(o);
    o->events_mask = m;
    return SL_OK;
}
u32 p_GetCallbackEventsMask(gaddr s, gp<u32> m) {
    OBJ(o);
    if (m.a) *m.get() = o->events_mask;
    return SL_OK;
}
u32 p_GetPosition(gaddr s, gp<u32> ms) {
    OBJ(o);
    if (ms.a) *ms.get() = static_cast<u32>(o->frac * 1000.0 / o->src_rate);
    return SL_OK;
}
u32 p_GetMarkerPosition(gaddr s, gp<u32> ms) {
    OBJ(o);
    if (ms.a) *ms.get() = 0;
    return SL_OK;
}
u32 p_GetPositionUpdatePeriod(gaddr s, gp<u32> ms) {
    OBJ(o);
    if (ms.a) *ms.get() = 1000;
    return SL_OK;
}

// ------------------------------------------------------------ SLVolumeItf

u32 v_SetVolumeLevel(gaddr s, i16 mb) {
    OBJ(o);
    o->volume_mb = mb;
    return SL_OK;
}
u32 v_GetVolumeLevel(gaddr s, gp<i16> mb) {
    OBJ(o);
    if (mb.a) *mb.get() = o->volume_mb;
    return SL_OK;
}
u32 v_GetMaxVolumeLevel(gaddr, gp<i16> mb) {
    if (mb.a) *mb.get() = 0;
    return SL_OK;
}
u32 v_SetMute(gaddr s, u32 m) {
    OBJ(o);
    o->mute = m != 0;
    return SL_OK;
}
u32 v_GetMute(gaddr s, gp<u32> m) {
    OBJ(o);
    if (m.a) *m.get() = o->mute;
    return SL_OK;
}
u32 v_IsEnabledStereoPosition(gaddr, gp<u32> en) {
    if (en.a) *en.get() = 0;
    return SL_OK;
}
u32 v_GetStereoPosition(gaddr, gp<i16> pos) {
    if (pos.a) *pos.get() = 0;
    return SL_OK;
}

// ------------------------------------------------------------ SLBufferQueueItf

u32 b_Enqueue(gaddr s, gaddr data, u32 size) {
    OBJ(o);
    o->queue.push_back(Buf{data, size, 0});
    return SL_OK;
}
u32 b_Clear(gaddr s) {
    OBJ(o);
    o->queue.clear();
    o->frac = 0;
    return SL_OK;
}
u32 b_GetState(gaddr s, gp<u32> st) {
    OBJ(o);
    if (st.a) {
        st.get()[0] = static_cast<u32>(o->queue.size());
        st.get()[1] = o->play_index;
    }
    return SL_OK;
}
u32 b_RegisterCallback(gaddr s, gaddr fn, gaddr ctx) {
    OBJ(o);
    o->bq_cb = fn;
    o->bq_ctx = ctx;
    return SL_OK;
}

// ------------------------------------------------------------ SLEffectSendItf

u32 fx_IsEnabled(gaddr, gaddr, gp<u32> en) {
    if (en.a) *en.get() = 0;
    return SL_OK;
}
u32 fx_GetDirectLevel(gaddr, gp<i16> lvl) {
    if (lvl.a) *lvl.get() = 0;
    return SL_OK;
}
u32 fx_GetSendLevel(gaddr, gaddr, gp<i16> lvl) {
    if (lvl.a) *lvl.get() = 0;
    return SL_OK;
}

// ------------------------------------------------------------ SLPlaybackRateItf

u32 r_SetRate(gaddr s, i16 permille) {
    OBJ(o);
    o->rate_permille = permille > 0 ? static_cast<u32>(permille) : 1000u;
    return SL_OK;
}
u32 r_GetRate(gaddr s, gp<i16> out) {
    OBJ(o);
    if (out.a) *out.get() = static_cast<i16>(o->rate_permille);
    return SL_OK;
}
u32 r_SetPropertyConstraints(gaddr s, u32) {
    OBJ(o);
    (void)o;
    return SL_OK;
}
u32 r_GetProperties(gaddr s, gp<u32> props) {
    OBJ(o);
    (void)o;
    if (props.a) *props.get() = 1;  // SL_RATEPROP_SILENTAUDIO / SL_RATEPROP_NOPITCHCORAUDIO
    return SL_OK;
}
u32 r_GetCapabilitiesOfRate(gaddr s, i16, gp<u32> caps) {
    OBJ(o);
    (void)o;
    if (caps.a) *caps.get() = 1;
    return SL_OK;
}
u32 r_GetRateRange(gaddr s, u8 index, gp<i16> min_rate, gp<i16> max_rate, gp<i16> step_size, gp<u32> caps) {
    OBJ(o);
    (void)o;
    if (index > 0) return SL_PARAM_INVALID;
    if (min_rate.a) *min_rate.get() = 500;
    if (max_rate.a) *max_rate.get() = 2000;
    if (step_size.a) *step_size.get() = 0;
    if (caps.a) *caps.get() = 1;
    return SL_OK;
}

// ------------------------------------------------------------ SLEngineCapabilitiesItf

u32 ec_QuerySupportedProfiles(gaddr, gp<u16> profiles) {
    if (profiles.a) *profiles.get() = 1;  // SL_PROFILES_PHONE
    return SL_OK;
}
u32 ec_QueryAvailableVoices(gaddr, u16, gp<i16> max_v, gp<u32> is_abs, gp<i16> free_v) {
    if (max_v.a) *max_v.get() = 32;
    if (is_abs.a) *is_abs.get() = 0;
    if (free_v.a) *free_v.get() = 32;
    return SL_OK;
}
u32 ec_QueryNumberOfMIDISynthesizers(gaddr, gp<i16> num) {
    if (num.a) *num.get() = 0;
    return SL_OK;
}
u32 ec_QueryAPIVersion(gaddr, gp<i16> major, gp<i16> minor, gp<i16> step) {
    if (major.a) *major.get() = 1;
    if (minor.a) *minor.get() = 0;
    if (step.a) *step.get() = 1;
    return SL_OK;
}
u32 ec_IsThreadSafe(gaddr, gp<u32> safe) {
    if (safe.a) *safe.get() = 1;
    return SL_OK;
}

// ------------------------------------------------------------ engine entry point + vtables

u32 sl_CreateEngine(gp<u32> engine, u32, gaddr, u32, gaddr, gaddr) {
    gaddr itf;
    u32 id = new_object(K_ENGINE, &itf);
    {
        std::lock_guard lk(g_mutex);
        find_obj(id)->realized = false;
    }
    *engine.get() = itf;
    return SL_OK;
}

gaddr T(const char* name, ThunkFn fn) { return cpu::register_thunk(name, fn); }

gaddr make_vtable(Itf type, std::vector<gaddr> slots) {
    gaddr vt = mem::gcalloc(static_cast<u32>(slots.size()), 4);
    for (size_t i = 0; i < slots.size(); ++i) wr<u32>(vt + 4 * static_cast<u32>(i), slots[i]);
    g_vtables[type] = vt;
    return vt;
}

}  // namespace

void register_opensl() {
    gaddr un = T("sl_unsupported", &adapt<u_Unsupported>);
    gaddr ok = T("sl_success", &adapt<u_Success>);

    make_vtable(I_OBJECT, {T("sl_o_Realize", &adapt<o_Realize>), T("sl_o_Resume", &adapt<o_Resume>),
                           T("sl_o_GetState", &adapt<o_GetState>), T("sl_o_GetInterface", &adapt<o_GetInterface>),
                           T("sl_o_RegisterCallback", &adapt<o_RegisterCallback>),
                           T("sl_o_AbortAsyncOperation", &adapt<o_AbortAsyncOperation>),
                           T("sl_o_Destroy", &adapt<o_Destroy>), T("sl_o_SetPriority", &adapt<o_SetPriority>),
                           T("sl_o_GetPriority", &adapt<o_GetPriority>),
                           T("sl_o_SetLossOfControlInterfaces", &adapt<o_SetLossOfControlInterfaces>)});

    make_vtable(I_ENGINE, {un, un, T("sl_e_CreateAudioPlayer", &adapt<e_CreateAudioPlayer>), un, un, un, un,
                           T("sl_e_CreateOutputMix", &adapt<e_CreateOutputMix>), un, un, ok, un, ok, un, ok});

    make_vtable(I_PLAY, {T("sl_p_SetPlayState", &adapt<p_SetPlayState>), T("sl_p_GetPlayState", &adapt<p_GetPlayState>),
                         T("sl_p_GetDuration", &adapt<p_GetDuration>), T("sl_p_GetPosition", &adapt<p_GetPosition>),
                         T("sl_p_RegisterCallback", &adapt<p_RegisterCallback>),
                         T("sl_p_SetCallbackEventsMask", &adapt<p_SetCallbackEventsMask>),
                         T("sl_p_GetCallbackEventsMask", &adapt<p_GetCallbackEventsMask>), ok, ok,
                         T("sl_p_GetMarkerPosition", &adapt<p_GetMarkerPosition>), ok,
                         T("sl_p_GetPositionUpdatePeriod", &adapt<p_GetPositionUpdatePeriod>)});

    make_vtable(I_VOLUME, {T("sl_v_SetVolumeLevel", &adapt<v_SetVolumeLevel>), T("sl_v_GetVolumeLevel", &adapt<v_GetVolumeLevel>),
                           T("sl_v_GetMaxVolumeLevel", &adapt<v_GetMaxVolumeLevel>), T("sl_v_SetMute", &adapt<v_SetMute>),
                           T("sl_v_GetMute", &adapt<v_GetMute>), ok,
                           T("sl_v_IsEnabledStereoPosition", &adapt<v_IsEnabledStereoPosition>), ok,
                           T("sl_v_GetStereoPosition", &adapt<v_GetStereoPosition>)});

    make_vtable(I_BQ, {T("sl_b_Enqueue", &adapt<b_Enqueue>), T("sl_b_Clear", &adapt<b_Clear>),
                       T("sl_b_GetState", &adapt<b_GetState>), T("sl_b_RegisterCallback", &adapt<b_RegisterCallback>)});

    make_vtable(I_EFFECT, {ok, T("sl_fx_IsEnabled", &adapt<fx_IsEnabled>), ok,
                           T("sl_fx_GetDirectLevel", &adapt<fx_GetDirectLevel>), ok,
                           T("sl_fx_GetSendLevel", &adapt<fx_GetSendLevel>)});

    make_vtable(I_RATE, {T("sl_r_SetRate", &adapt<r_SetRate>), T("sl_r_GetRate", &adapt<r_GetRate>),
                         T("sl_r_SetPropertyConstraints", &adapt<r_SetPropertyConstraints>),
                         T("sl_r_GetProperties", &adapt<r_GetProperties>),
                         T("sl_r_GetCapabilitiesOfRate", &adapt<r_GetCapabilitiesOfRate>),
                         T("sl_r_GetRateRange", &adapt<r_GetRateRange>)});

    make_vtable(I_CAPS, {T("sl_ec_QuerySupportedProfiles", &adapt<ec_QuerySupportedProfiles>),
                         T("sl_ec_QueryAvailableVoices", &adapt<ec_QueryAvailableVoices>),
                         T("sl_ec_QueryNumberOfMIDISynthesizers", &adapt<ec_QueryNumberOfMIDISynthesizers>),
                         T("sl_ec_QueryAPIVersion", &adapt<ec_QueryAPIVersion>), ok, ok,
                         T("sl_ec_IsThreadSafe", &adapt<ec_IsThreadSafe>)});

    // Distinct interface IDs: the guest compares them by content through GetInterface.
    const char* names[] = {"", "SL_IID_ENGINE", "SL_IID_PLAY", "SL_IID_VOLUME", "SL_IID_BUFFERQUEUE",
                           "SL_IID_EFFECTSEND", "SL_IID_PLAYBACKRATE", "SL_IID_ENGINECAPABILITIES",
                           "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"};
    for (int i = 1; i <= I_COUNT; ++i) {
        auto& b = g_iid_bytes[static_cast<size_t>(i)];
        for (size_t k = 0; k < 16; ++k) b[k] = static_cast<u8>(0xA0 + i * 11 + k * 3);
        gaddr g = mem::gcalloc(1, 16);
        std::memcpy(hp(g), b.data(), 16);
        gaddr var = cpu::find_data(names[i]);
        if (!var) var = mem::gmalloc(4);
        wr<u32>(var, g);
        cpu::register_data(names[i], var);
    }
    // I_OBJECT (index 0) never matches a real request
    std::fill(g_iid_bytes[0].begin(), g_iid_bytes[0].end(), 0xEE);

    REG(slCreateEngine, sl_CreateEngine);
}

// The background music (not an OpenSL player) needs the output to exist before any sound effect does.
void start_audio_mixer() { ensure_mixer(); }

}  // namespace zb::android
