#include "jni_env.h"
#include "audio/music_player.h"
#include "core/thunk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace zb::jni {

namespace {

// -----------------------------------------------------------------------------
// Internal Mock Handle Table
// -----------------------------------------------------------------------------
struct Slot {
    MockObject obj;
    ObjectKind kind = ObjectKind::Free;
    u32 gen = 0;
    u32 next_free = UINT32_MAX;
};

std::mutex g_slot_lock;
std::vector<Slot> g_slots;
u32 g_free_head = UINT32_MAX;

using Frame = std::vector<std::pair<u32, u32>>; // (slot_index, gen)
thread_local std::vector<Frame> t_frames;
thread_local u32 t_pending_exception = 0;

inline u32 handle_of(u32 idx) {
    return (idx << 2) | 2; // Tag bits 0b10 to avoid 0 and 4-byte aligned pointers
}

inline bool idx_of(u32 h, u32* idx) {
    if ((h & 3) != 2) return false;
    *idx = h >> 2;
    return true;
}

u32 alloc_slot(ObjectKind kind, ObjectType type, u32* gen_out) {
    std::lock_guard<std::mutex> lk(g_slot_lock);
    u32 idx = 0;
    if (g_free_head != UINT32_MAX) {
        idx = g_free_head;
        g_free_head = g_slots[idx].next_free;
    } else {
        idx = static_cast<u32>(g_slots.size());
        g_slots.emplace_back();
    }

    Slot& s = g_slots[idx];
    s.kind = kind;
    s.obj = MockObject{};
    s.obj.type = type;
    s.gen++;
    if (gen_out) *gen_out = s.gen;
    return idx;
}

void free_slot_locked(u32 idx) {
    Slot& s = g_slots[idx];
    s.kind = ObjectKind::Free;
    s.obj = MockObject{};
    s.next_free = g_free_head;
    g_free_head = idx;
}

ObjectKind kind_of_handle(u32 h) {
    u32 idx = 0;
    if (!idx_of(h, &idx)) return ObjectKind::Free;
    std::lock_guard<std::mutex> lk(g_slot_lock);
    return (idx < g_slots.size()) ? g_slots[idx].kind : ObjectKind::Free;
}

void release_frame(const Frame& f) {
    std::lock_guard<std::mutex> lk(g_slot_lock);
    for (auto [idx, gen] : f) {
        if (idx < g_slots.size() && g_slots[idx].gen == gen && g_slots[idx].kind == ObjectKind::Local) {
            free_slot_locked(idx);
        }
    }
}

Frame& current_frame() {
    if (t_frames.empty()) {
        t_frames.emplace_back();
    }
    return t_frames.back();
}

u32 clone_to_local(u32 src_handle) {
    if (!src_handle) return 0;
    MockObject copy;
    {
        u32 idx = 0;
        if (!idx_of(src_handle, &idx)) return 0;
        std::lock_guard<std::mutex> lk(g_slot_lock);
        if (idx >= g_slots.size() || g_slots[idx].kind == ObjectKind::Free) return 0;
        copy = g_slots[idx].obj;
    }
    u32 gen = 0;
    u32 new_idx = alloc_slot(ObjectKind::Local, copy.type, &gen);
    {
        std::lock_guard<std::mutex> lk(g_slot_lock);
        g_slots[new_idx].obj = std::move(copy);
    }
    current_frame().emplace_back(new_idx, gen);
    return handle_of(new_idx);
}

// -----------------------------------------------------------------------------
// Method & Field Registry
// -----------------------------------------------------------------------------
struct MethodMeta {
    MethodSignature sig;
    MockMethodHandler custom_handler;
};

struct FieldMeta {
    std::string class_name;
    std::string field_name;
    std::string signature;
};

std::mutex g_meta_lock;
std::vector<MethodMeta> g_methods;
std::unordered_map<std::string, u32> g_method_map; // "class.name(sig)" -> handle

std::vector<FieldMeta> g_fields;
std::unordered_map<std::string, u32> g_field_map;  // "class.name:sig" -> handle
std::unordered_map<u32, jvalue> g_static_fields;

bool parse_signature(const char* sig_str, std::string* args, char* ret) {
    if (!sig_str || *sig_str != '(') return false;
    const char* p = sig_str + 1;
    while (*p && *p != ')') {
        if (*p == '[') {
            while (*p == '[') p++;
            if (*p == 'L') {
                while (*p && *p != ';') p++;
            }
            if (*p) p++;
            args->push_back('L');
        } else if (*p == 'L') {
            while (*p && *p != ';') p++;
            if (*p) p++;
            args->push_back('L');
        } else {
            args->push_back(*p++);
        }
    }
    if (*p != ')') return false;
    p++;
    *ret = (*p == '[') ? 'L' : *p;
    return true;
}

u32 register_method(const char* cls, const char* name, const char* sig, MockMethodHandler handler = nullptr) {
    std::lock_guard<std::mutex> lk(g_meta_lock);
    std::string c_str = cls ? cls : "";
    std::string n_str = name ? name : "";
    std::string s_str = sig ? sig : "";
    std::string key = c_str + "." + n_str + s_str;
    auto it = g_method_map.find(key);
    if (it != g_method_map.end()) {
        if (handler && !g_methods[(it->second >> 3) - 1].custom_handler) {
            g_methods[(it->second >> 3) - 1].custom_handler = std::move(handler);
        }
        return it->second;
    }

    MethodMeta meta;
    meta.sig.class_name = c_str;
    meta.sig.method_name = n_str;
    meta.sig.signature = s_str;
    parse_signature(s_str.c_str(), &meta.sig.arg_types, &meta.sig.return_type);
    meta.custom_handler = std::move(handler);

    u32 idx = static_cast<u32>(g_methods.size());
    g_methods.push_back(std::move(meta));
    u32 handle = (idx + 1) << 3;
    g_method_map[key] = handle;
    return handle;
}

MethodMeta get_method(u32 handle) {
    std::lock_guard<std::mutex> lk(g_meta_lock);
    u32 idx = (handle >> 3) - 1;
    if (handle == 0 || idx >= g_methods.size()) {
        fatal("Invalid jmethodID 0x%08x", handle);
    }
    return g_methods[idx];
}

u32 register_field(const char* cls, const char* name, const char* sig) {
    std::lock_guard<std::mutex> lk(g_meta_lock);
    std::string c_str = cls ? cls : "";
    std::string n_str = name ? name : "";
    std::string s_str = sig ? sig : "";
    std::string key = c_str + "." + n_str + ":" + s_str;
    auto it = g_field_map.find(key);
    if (it != g_field_map.end()) {
        return it->second;
    }

    u32 idx = static_cast<u32>(g_fields.size());
    g_fields.push_back(FieldMeta{c_str, n_str, s_str});
    u32 handle = (idx + 1) << 3;
    g_field_map[key] = handle;

    // Populate default static values for common Android SDK fields
    if (n_str == "SDK_INT" && s_str == "I") {
        jvalue v{};
        v.i = 24;
        g_static_fields[handle] = v;
    }
    return handle;
}

// -----------------------------------------------------------------------------
// Global Subsystem State
// -----------------------------------------------------------------------------
OfflineConfig g_offline_config;
NativeAddresses g_native_addrs;
bool g_bias_resolved = false;
bool g_natives_bootstrapped = false;

gaddr g_guest_vm_ptr = 0;
gaddr g_guest_env_table = 0;
gaddr g_main_guest_env = 0;
std::array<gaddr, 233> g_vtable_thunks{};

void ensure_bias_from_caller(gaddr caller_addr) {
    if (g_bias_resolved || caller_addr < 0x00100000u) return;
    gaddr page = caller_addr & ~0xFFFu;
    gaddr min_page = (page > 0x00300000u) ? (page - 0x00300000u) : 0x1000u;
    for (gaddr p = page; p >= min_page; p -= 0x1000u) {
        if (mem::is_mapped(p) && mem::is_mapped(p + 15)) {
            const u8* hdr = hp<const u8>(p);
            if (hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F' &&
                hdr[4] == 1 && hdr[5] == 1) {
                resolve_native_addresses(p);
                return;
            }
        }
        if (p < 0x1000u) break;
    }
}

// Known IAP product catalog (19 items from binary extraction)
const std::vector<std::pair<std::string, std::string>> kIapCatalog = {
    {"com.playstation.runsackboyrun.autoshield", "Auto-Shield"},
    {"com.playstation.runsackboyrun.doublebubbles", "Double Bubbles"},
    {"com.playstation.runsackboyrun.freeboost", "Free Boost"},
    {"com.playstation.runsackboyrun.santaclaus", "Santa Costume"},
    {"com.playstation.runsackboyrun.bumperpack", "Bumper Pack"},
    {"com.playstation.runsackboyrun.megapack", "Mega Pack"},
    {"com.playstation.runsackboyrun.stickerchance", "Sticker Chance"},
    {"com.playstation.runsackboyrun.bubblepack1", "Pocket of Bubbles"},
    {"com.playstation.runsackboyrun.bubblepack2", "Pouch of Bubbles"},
    {"com.playstation.runsackboyrun.bubblepack3", "Sack of Bubbles"},
    {"com.playstation.runsackboyrun.bubblepack4", "Chest of Bubbles"},
    {"com.playstation.runsackboyrun.bubblepack5", "Vault of Bubbles"},
    {"com.playstation.runsackboyrun.bubblepack6", "Mountain of Bubbles"},
    {"com.playstation.runsackboyrun.bubblepacksale", "Special Sale Bubbles"},
    {"com.playstation.runsackboyrun.heartpack1", "1 Heart"},
    {"com.playstation.runsackboyrun.heartpack2", "5 Hearts"},
    {"com.playstation.runsackboyrun.heartpack3", "12 Hearts"},
    {"com.playstation.runsackboyrun.heartpack4", "25 Hearts"},
    {"com.playstation.runsackboyrun.heartpack5", "60 Hearts"}
};

// -----------------------------------------------------------------------------
// Mock Dispatch Handler for Dalvik / Android / RSBR JNI Methods
// -----------------------------------------------------------------------------
u64 dispatch_mock_method(const MethodSignature& sig, u32 self_obj, u32 /*cls_obj*/, const std::vector<jvalue>& args) {
    const std::string& name = sig.method_name;
    if (g_debug) {
        static std::mutex m;
        static std::unordered_map<std::string, int> seen;
        std::lock_guard lk(m);
        if (++seen[name] <= 3) ZLOGD("[Java] %s.%s%s", sig.class_name.c_str(), name.c_str(), sig.signature.c_str());
    }

    // --- ClassLoader & Reflection / File / Context ---
    if (name == "getClassLoader") {
        return create_local_class("java/lang/ClassLoader");
    }
    if (name == "loadClass" || name == "findClass") {
        const char* target_cls = (!args.empty() && args[0].l) ? get_string_utf(args[0].l) : "java/lang/Object";
        std::string normalized = target_cls ? target_cls : "java/lang/Object";
        for (char& ch : normalized) {
            if (ch == '.') ch = '/';
        }
        return create_local_class(normalized);
    }
    if (name == "getExternalStorageDirectory") {
        u32 h = create_local_object(ObjectType::Generic);
        if (MockObject* o = get_object(h)) {
            o->class_name = "java/io/File";
            o->str_val = "/storage/emulated/0";
        }
        return h;
    }
    if (name == "getFilesDir" || name == "getExternalFilesDir") {
        u32 h = create_local_object(ObjectType::Generic);
        if (MockObject* o = get_object(h)) {
            o->class_name = "java/io/File";
            o->str_val = "/data/data/com.playstation.runsackboyrun/files";
        }
        return h;
    }
    if (name == "getCacheDir" || name == "getExternalCacheDir") {
        u32 h = create_local_object(ObjectType::Generic);
        if (MockObject* o = get_object(h)) {
            o->class_name = "java/io/File";
            o->str_val = "/data/data/com.playstation.runsackboyrun/cache";
        }
        return h;
    }
    if (name == "getPath" || name == "getAbsolutePath" || name == "getCanonicalPath") {
        MockObject* o = get_object(self_obj);
        if (o && !o->str_val.empty()) {
            return create_local_string(o->str_val);
        }
        return create_local_string("/storage/emulated/0");
    }
    if (name == "getName") {
        MockObject* o = get_object(self_obj);
        if (o && !o->class_name.empty()) {
            return create_local_string(o->class_name);
        }
        if (o && !o->str_val.empty()) {
            return create_local_string(o->str_val);
        }
        return create_local_string("java.lang.Object");
    }
    if (name == "toString") {
        MockObject* o = get_object(self_obj);
        if (o && !o->str_val.empty()) {
            return create_local_string(o->str_val);
        }
        return create_local_string("");
    }
    if (name == "getPackageName") {
        return create_local_string("com.playstation.runsackboyrun");
    }
    if (name == "GetContext" || name == "GetInstance" || name == "getApplicationContext" || name == "getBaseContext") {
        return create_local_class("com/playstation/runsackboyrun/RSBR");
    }

    // --- System & Display ---
    if (name == "GetDisplayDensity") {
        float d = g_offline_config.display_density;
        u32 b;
        std::memcpy(&b, &d, 4);
        return b;
    }
    if (name == "GetServerTime") {
        return static_cast<u32>(std::time(nullptr));
    }
    if (name == "CheckServerTime") {
        return 0;
    }
    if (name == "IsInternetAvailableAndroid") {
        // The game's servers (PSN, Facebook, downloads) are gone: report no connection, as the original
        // does on a device without network.
        return 0;
    }
    if (name == "droid_CheckAndroid43") {
        return 1;
    }
    if (name == "droid_GetRegion" || name == "GetRegion") {
        return create_local_string(g_offline_config.language + "-" + g_offline_config.country);
    }
    if (name == "fetchDeviceID") {
        return create_local_string(g_offline_config.device_id);
    }
    if (name == "ShowSystemMsgAndroid") {
        if (!args.empty() && args[0].l) {
            ZLOGI("[SystemMsg] %s", get_string_utf(args[0].l));
        }
        return 0;
    }
    if (name == "OpenURL") {
        if (!args.empty() && args[0].l) {
            ZLOGI("[OpenURL] %s", get_string_utf(args[0].l));
        }
        return 0;
    }
    if (name == "SetupLocalAndroidNotification") {
        return 0;
    }

    // --- Music & Audio ---
    if (name == "PlayMusic") {
        const char* track = (!args.empty() && args[0].l) ? get_string_utf(args[0].l) : "unknown";
        bool loop = (args.size() > 1) ? (args[1].z != 0) : true;
        ZLOGI("[Music] PlayMusic('%s', loop=%d)", track, loop);
        return audio::music::play(track, loop) ? 1 : 0;
    }
    if (name == "PauseMusic") {
        ZLOGD("[Music] PauseMusic()");
        audio::music::pause();
        return 0;
    }
    if (name == "ResumeMusic") {
        ZLOGD("[Music] ResumeMusic()");
        audio::music::resume();
        return 0;
    }
    if (name == "RestartMusic") {
        ZLOGD("[Music] RestartMusic()");
        audio::music::restart();
        return 0;
    }
    if (name == "StopMusic") {
        ZLOGD("[Music] StopMusic()");
        audio::music::stop();
        return 0;
    }
    if (name == "SetMusicVolume") {
        if (!args.empty()) audio::music::set_volume(args[0].f);
        return 0;
    }

    // --- Canvas / Text Rendering Fallback ---
    if (name == "RenderTextWidth") {
        const char* text = (!args.empty() && args[0].l) ? get_string_utf(args[0].l) : "";
        float size = (args.size() > 2) ? args[2].f : 24.0f;
        int width = static_cast<int>(std::strlen(text) * size * 0.6f);
        return static_cast<u32>(width);
    }
    if (name == "RenderText") {
        // Java-side text rasterisation (Canvas + assets/fonts) is not implemented: the game draws its usual
        // UI text with its own bitmap fonts and was not seen calling this in English. Report it if it does.
        static std::atomic<int> calls{0};
        if (args.size() >= 6 && ++calls <= 5) {
            ZLOGW("[RenderText] not implemented, text not drawn: '%s' font='%s' size=%.1f tex=%d",
                  args[0].l ? get_string_utf(args[0].l) : "", args[1].l ? get_string_utf(args[1].l) : "", args[2].f,
                  args[3].i);
        }
        return 0;
    }

    // --- In-App Purchases (IAP) & Perks ---
    if (name == "iap_RequestProductData") {
        return 1;
    }
    if (name == "iap_ProductDataAvailable") {
        return 1;
    }
    if (name == "iap_GetProductCount") {
        return static_cast<u32>(kIapCatalog.size());
    }
    if (name == "iap_FindProduct") {
        if (!args.empty() && args[0].l) {
            std::string sku = get_string_utf(args[0].l);
            for (size_t i = 0; i < kIapCatalog.size(); ++i) {
                if (kIapCatalog[i].first == sku) {
                    return static_cast<u32>(i);
                }
            }
        }
        return static_cast<u32>(-1);
    }
    if (name == "iap_GetProductPriceString") {
        return create_local_string("FREE");
    }
    // The store is gone: a requested purchase succeeds locally, once, until the game resets it.
    static std::atomic<bool> purchase_pending{false};
    if (name == "iap_RequestPurchase") {
        purchase_pending = true;
        return 1;
    }
    if (name == "iap_PurchaseAvailable") {
        return 1;
    }
    if (name == "iap_PurchaseSucceeded") {
        return purchase_pending.load() ? 1 : 0;
    }
    if (name == "iap_PurchaseFailed") {
        return 0;
    }
    if (name == "iap_PurchaseCancelled") {
        return 0;
    }
    if (name == "iap_ResetPurchase") {
        purchase_pending = false;
        return 0;
    }

    // --- PlayStation Network (PSN) & Poll Command Stubs ---
    if (name == "droid_PSNGetLoginState") {
        return 0; // Offline
    }
    if (name == "droid_PSNSetLoginState" || name == "CompanionUtilSignin" || name == "droid_ResetVoucherRead") {
        return 0;
    }
    if (name == "droid_NetRequestVoucherSucceeded" || name == "droid_NetFileDownloadSucceeded") {
        return 0;
    }
    if (name == "droid_NetRequestVoucherFailed" || name == "droid_NetFileDownloadFailed" || name == "droid_NetFileDownloadEnd") {
        return 1;
    }
    if (name == "droid_NetRequestGetVoucherData") {
        return create_local_string("");
    }
    if (name == "droid_NetFileDownloadGetData") {
        return create_local_byte_array(nullptr, 0);
    }
    if (name == "droid_VoucherRedeem" || name == "droid_NetFileDownloadBegin") {
        return 0;
    }
    if (name == "HariKari" || name == "Signin" || name == "PlayServices" ||
        name == "Ranking" || name == "PostAchievement" || name == "UploadState" || name == "Expired") {
        return 0;
    }

    // --- Facebook Offline Stubs ---
    if (name == "facebook_HasConnection" || name == "facebook_LoggingIn" ||
        name == "facebook_HasPublishPermissions" || name == "facebook_RequestingPermissions" ||
        name == "facebook_GetBragStatus" || name == "facebook_GetBragSuccess" ||
        name == "facebook_GetProfilePictureDataReady") {
        return 0; // Offline
    }
    if (name == "facebook_GotLeaderboard" || name == "facebook_GetNumLeaderBoardResults" ||
        name == "facebook_GetLeaderboardID" || name == "facebook_GetLeaderboardScore") {
        return 0;
    }
    if (name == "facebook_GetMyLeaderboardPosition") {
        return static_cast<u32>(-1);
    }
    if (name == "facebook_GetLeaderboardName") {
        return create_local_string("");
    }
    if (name == "facebook_GetFacebookProfilePixels") {
        return create_local_int_array(nullptr, 0);
    }
    if (name.rfind("facebook_", 0) == 0) {
        return 0; // All other facebook actions are silent no-ops
    }

    // --- Analytics & Adverts ---
    if (name == "advert_isReady") {
        return 0;
    }
    if (name == "advert_getReward") {
        // The original returns true once after a rewarded ad was watched. There is no ad network any more:
        // answering true here made the game grant a reward (and save) on every poll.
        return 0;
    }
    if (name.rfind("Analytics", 0) == 0 || name.rfind("advert_", 0) == 0) {
        return 0;
    }

    // Return default empty values based on return signature
    if (sig.return_type == 'L') {
        if (sig.signature.find(")Ljava/lang/String;") != std::string::npos) {
            return create_local_string("");
        }
        if (sig.signature.find(")[B") != std::string::npos) {
            return create_local_byte_array(nullptr, 0);
        }
        if (sig.signature.find(")[I") != std::string::npos) {
            return create_local_int_array(nullptr, 0);
        }
    }

    ZLOGW("[JNI Mock] Unhandled method %s.%s%s", sig.class_name.c_str(), name.c_str(), sig.signature.c_str());
    return 0;
}

// -----------------------------------------------------------------------------
// AAPCS Argument Unpacker for Variadic / va_list / jvalue* JNI Calls
// -----------------------------------------------------------------------------
struct VaReader {
    u32 ap;
    u32 read32() {
        u32 v = rd<u32>(ap);
        ap += 4;
        return v;
    }
    u64 read64() {
        ap = (ap + 7) & ~7u; // 8-byte alignment for 64-bit types
        u64 v = rd<u64>(ap);
        ap += 8;
        return v;
    }
};

std::vector<jvalue> decode_va_arguments(const std::string& arg_types, u32 ap) {
    std::vector<jvalue> values(arg_types.size());
    VaReader va{ap};
    for (size_t i = 0; i < arg_types.size(); ++i) {
        switch (arg_types[i]) {
            case 'Z': values[i].z = static_cast<jboolean>(va.read32()); break;
            case 'B': values[i].b = static_cast<jbyte>(va.read32()); break;
            case 'C': values[i].c = static_cast<jchar>(va.read32()); break;
            case 'S': values[i].s = static_cast<jshort>(va.read32()); break;
            case 'I': values[i].i = static_cast<jint>(va.read32()); break;
            case 'J': values[i].j = static_cast<jlong>(va.read64()); break;
            case 'F': {
                u64 w = va.read64(); // Floats promoted to double in C/C++ variadics
                double d;
                std::memcpy(&d, &w, 8);
                values[i].f = static_cast<jfloat>(d);
                break;
            }
            case 'D': {
                u64 w = va.read64();
                std::memcpy(&values[i].d, &w, 8);
                break;
            }
            default:
                values[i].l = va.read32();
                break;
        }
    }
    return values;
}

std::vector<jvalue> decode_jvalue_array(const std::string& arg_types, gaddr arr) {
    std::vector<jvalue> values(arg_types.size());
    if (!arr) return values;
    for (size_t i = 0; i < arg_types.size(); ++i) {
        gaddr a = arr + static_cast<u32>(i * 8);
        switch (arg_types[i]) {
            case 'Z': values[i].z = rd<u8>(a); break;
            case 'B': values[i].b = rd<i8>(a); break;
            case 'C': values[i].c = rd<u16>(a); break;
            case 'S': values[i].s = rd<i16>(a); break;
            case 'I': values[i].i = rd<i32>(a); break;
            case 'J': values[i].j = rd<i64>(a); break;
            case 'F': values[i].f = rd<float>(a); break;
            case 'D': values[i].d = rd<double>(a); break;
            default:  values[i].l = rd<u32>(a); break;
        }
    }
    return values;
}

enum CallKind { kVirtual = 0, kNonvirtual = 1, kStatic = 2 };
enum VarKind  { kDots = 0, kVaList = 1, kArray = 2 };

template <char R, int Kind, int Var>
void call_thunk(JitCtx& j) {
    ensure_bias_from_caller(j.r()[14]);
    ArgReader r(j);
    r.w32(); // env
    u32 obj = 0;
    u32 cls = 0;
    if constexpr (Kind != kStatic) obj = r.w32();
    if constexpr (Kind != kVirtual) cls = r.w32();
    u32 mid = r.w32();
    MethodMeta meta = get_method(mid);

    std::vector<jvalue> args;
    if constexpr (Var == kDots) {
        args = decode_va_arguments(meta.sig.arg_types, r.va_list_here());
    } else if constexpr (Var == kVaList) {
        args = decode_va_arguments(meta.sig.arg_types, r.w32());
    } else {
        args = decode_jvalue_array(meta.sig.arg_types, r.w32());
    }

    u64 res = meta.custom_handler ? meta.custom_handler(meta.sig, args)
                                  : dispatch_mock_method(meta.sig, obj, cls, args);

    if constexpr (R == 'V') {
        (void)res;
    } else if constexpr (R == 'J' || R == 'D') {
        j.set_ret64(res);
    } else if constexpr (R == 'Z') {
        j.set_ret(static_cast<u32>(res & 0xFF));
    } else if constexpr (R == 'B') {
        j.set_ret(static_cast<u32>(static_cast<i32>(static_cast<i8>(res))));
    } else if constexpr (R == 'C') {
        j.set_ret(static_cast<u32>(res & 0xFFFF));
    } else if constexpr (R == 'S') {
        j.set_ret(static_cast<u32>(static_cast<i32>(static_cast<i16>(res))));
    } else {
        j.set_ret(static_cast<u32>(res));
    }
}

template <int Var>
void new_object_thunk(JitCtx& j) {
    ensure_bias_from_caller(j.r()[14]);
    ArgReader r(j);
    r.w32(); // env
    u32 cls = r.w32();
    u32 mid = r.w32();
    MethodMeta meta = get_method(mid);

    std::vector<jvalue> args;
    if constexpr (Var == kDots) {
        args = decode_va_arguments(meta.sig.arg_types, r.va_list_here());
    } else if constexpr (Var == kVaList) {
        args = decode_va_arguments(meta.sig.arg_types, r.w32());
    } else {
        args = decode_jvalue_array(meta.sig.arg_types, r.w32());
    }

    MockObject* cls_obj = get_object(cls);
    std::string cname = cls_obj ? cls_obj->class_name : meta.sig.class_name;
    u32 h = create_local_object(ObjectType::Generic);
    if (MockObject* o = get_object(h)) {
        o->class_name = cname;
        if (!args.empty() && args[0].l) {
            o->str_val = get_string_utf(args[0].l);
        }
    }
    j.set_ret(h);
}

// -----------------------------------------------------------------------------
// Thunk Implementations of JNINativeInterface (Indices 4..232)
// -----------------------------------------------------------------------------
void thunk_GetVersion(JitCtx& j) {
    j.set_ret(JNI_VERSION_1_6);
}

void thunk_DefineClass(JitCtx& j) {
    const char* name = j.arg(1) ? hp<const char>(j.arg(1)) : "";
    ZLOGW("JNI DefineClass('%s') is not supported", name);
    j.set_ret(0);
}

void thunk_FindClass(JitCtx& j) {
    ensure_bias_from_caller(j.r()[14]);
    gaddr name_ptr = j.arg(1);
    const char* name = name_ptr ? hp<const char>(name_ptr) : "";
    ZLOGD("FindClass('%s')", name ? name : "null");
    j.set_ret(create_local_class(name ? name : ""));
}

void thunk_FromReflectedMethod(JitCtx& j) {
    j.set_ret(register_method("java/lang/Object", "reflectedMethod", "()V"));
}

void thunk_FromReflectedField(JitCtx& j) {
    j.set_ret(register_field("java/lang/Object", "reflectedField", "Ljava/lang/Object;"));
}

void thunk_ToReflectedMethod(JitCtx& j) {
    u32 h = create_local_object(ObjectType::Generic);
    if (MockObject* o = get_object(h)) o->class_name = "java/lang/reflect/Method";
    j.set_ret(h);
}

void thunk_GetSuperclass(JitCtx& j) {
    MockObject* cls = get_object(j.arg(1));
    if (!cls || cls->class_name == "java/lang/Object") {
        j.set_ret(0);
        return;
    }
    if (cls->class_name == "com/playstation/runsackboyrun/RSBR") {
        j.set_ret(create_local_class("android/app/NativeActivity"));
        return;
    }
    j.set_ret(create_local_class("java/lang/Object"));
}

void thunk_IsAssignableFrom(JitCtx& j) {
    j.set_ret(JNI_TRUE);
}

void thunk_ToReflectedField(JitCtx& j) {
    u32 h = create_local_object(ObjectType::Generic);
    if (MockObject* o = get_object(h)) o->class_name = "java/lang/reflect/Field";
    j.set_ret(h);
}

void thunk_Throw(JitCtx& j) {
    t_pending_exception = j.arg(1);
    j.set_ret(JNI_OK);
}

void thunk_ThrowNew(JitCtx& j) {
    const char* msg = j.arg(2) ? hp<const char>(j.arg(2)) : "JNI Exception";
    u32 h = create_local_object(ObjectType::Throwable);
    if (MockObject* o = get_object(h)) {
        o->str_val = msg ? msg : "";
    }
    t_pending_exception = h;
    j.set_ret(JNI_OK);
}

void thunk_ExceptionOccurred(JitCtx& j) {
    ensure_bias_from_caller(j.r()[14]);
    j.set_ret(t_pending_exception);
}

void thunk_ExceptionDescribe(JitCtx& j) {
    if (t_pending_exception) {
        MockObject* o = get_object(t_pending_exception);
        ZLOGW("JNI ExceptionDescribe: %s", (o && !o->str_val.empty()) ? o->str_val.c_str() : "pending exception");
        t_pending_exception = 0;
    }
    (void)j;
}

void thunk_ExceptionClear(JitCtx& j) {
    t_pending_exception = 0;
    (void)j;
}

void thunk_FatalError(JitCtx& j) {
    const char* msg = j.arg(1) ? hp<const char>(j.arg(1)) : "FatalError";
    cpu::dump_state(j, "JNI FatalError");
    fatal("Guest JNI FatalError: %s", msg);
}

void thunk_PushLocalFrame(JitCtx& j) {
    t_frames.emplace_back();
    j.set_ret(JNI_OK);
}

void thunk_PopLocalFrame(JitCtx& j) {
    u32 res_arg = j.arg(1);
    MockObject saved;
    bool has_res = false;
    if (res_arg) {
        if (MockObject* o = get_object(res_arg)) {
            saved = *o;
            has_res = true;
        }
    }
    if (!t_frames.empty()) {
        release_frame(t_frames.back());
        t_frames.pop_back();
    }
    if (!has_res) {
        j.set_ret(0);
        return;
    }
    u32 h = create_local_object(saved.type);
    if (MockObject* o = get_object(h)) {
        *o = std::move(saved);
    }
    j.set_ret(h);
}

void thunk_NewGlobalRef(JitCtx& j) {
    j.set_ret(to_global_handle(j.arg(1)));
}

void thunk_DeleteGlobalRef(JitCtx& j) {
    u32 h = j.arg(1);
    if (kind_of_handle(h) == ObjectKind::Global) {
        delete_handle(h);
    }
}

void thunk_DeleteLocalRef(JitCtx& j) {
    u32 h = j.arg(1);
    if (kind_of_handle(h) == ObjectKind::Local) {
        delete_handle(h);
    }
}

void thunk_IsSameObject(JitCtx& j) {
    j.set_ret(j.arg(1) == j.arg(2) ? JNI_TRUE : JNI_FALSE);
}

void thunk_NewLocalRef(JitCtx& j) {
    j.set_ret(clone_to_local(j.arg(1)));
}

void thunk_EnsureLocalCapacity(JitCtx& j) {
    j.set_ret(JNI_OK);
}

void thunk_AllocObject(JitCtx& j) {
    MockObject* cls = get_object(j.arg(1));
    std::string cname = cls ? cls->class_name : "java/lang/Object";
    u32 h = create_local_object(ObjectType::Generic);
    if (MockObject* o = get_object(h)) o->class_name = cname;
    j.set_ret(h);
}

void thunk_GetObjectClass(JitCtx& j) {
    MockObject* obj = get_object(j.arg(1));
    std::string cname = (obj && !obj->class_name.empty()) ? obj->class_name : "java/lang/Object";
    j.set_ret(create_local_class(cname));
}

void thunk_IsInstanceOf(JitCtx& j) {
    j.set_ret(j.arg(1) != 0 ? JNI_TRUE : JNI_FALSE);
}

void thunk_GetMethodID(JitCtx& j) {
    ensure_bias_from_caller(j.r()[14]);
    u32 cls = j.arg(1);
    const char* name = j.arg(2) ? hp<const char>(j.arg(2)) : "";
    const char* sig = j.arg(3) ? hp<const char>(j.arg(3)) : "";
    MockObject* obj = get_object(cls);
    const char* cname = (obj && !obj->class_name.empty()) ? obj->class_name.c_str() : "unknown";
    ZLOGD("GetMethodID('%s', '%s', '%s')", cname, name, sig);
    u32 mid = register_method(cname, name, sig);
    j.set_ret(mid);
}

void thunk_GetStaticMethodID(JitCtx& j) {
    ensure_bias_from_caller(j.r()[14]);
    u32 cls = j.arg(1);
    const char* name = j.arg(2) ? hp<const char>(j.arg(2)) : "";
    const char* sig = j.arg(3) ? hp<const char>(j.arg(3)) : "";
    MockObject* obj = get_object(cls);
    const char* cname = (obj && !obj->class_name.empty()) ? obj->class_name.c_str() : "unknown";
    ZLOGD("GetStaticMethodID('%s', '%s', '%s')", cname, name, sig);
    u32 mid = register_method(cname, name, sig);
    j.set_ret(mid);

    // At the end of engine_InitJavaClasses(), bootstrap SetFilesDir and SetUniqueDeviceID
    // before AppMain::ResetDeviceID() and AppMain::Initialise() execute.
    if (g_bias_resolved && !g_natives_bootstrapped && name && std::strcmp(name, "advert_getReward") == 0) {
        g_natives_bootstrapped = true;
        call_SetFilesDir("data/data/com.playstation.runsackboyrun/files", "ID0");
        call_SetUniqueDeviceID(g_offline_config.device_id);
    }
}

// -----------------------------------------------------------------------------
// Instance & Static Fields (Indices 94..112, 144..162)
// -----------------------------------------------------------------------------
void thunk_GetFieldID(JitCtx& j) {
    MockObject* cls = get_object(j.arg(1));
    const char* cname = (cls && !cls->class_name.empty()) ? cls->class_name.c_str() : "unknown";
    const char* name = j.arg(2) ? hp<const char>(j.arg(2)) : "";
    const char* sig = j.arg(3) ? hp<const char>(j.arg(3)) : "";
    j.set_ret(register_field(cname, name, sig));
}

void thunk_GetStaticFieldID(JitCtx& j) {
    MockObject* cls = get_object(j.arg(1));
    const char* cname = (cls && !cls->class_name.empty()) ? cls->class_name.c_str() : "unknown";
    const char* name = j.arg(2) ? hp<const char>(j.arg(2)) : "";
    const char* sig = j.arg(3) ? hp<const char>(j.arg(3)) : "";
    j.set_ret(register_field(cname, name, sig));
}

template <char T, bool IsStatic>
void get_field_thunk(JitCtx& j) {
    u32 target = j.arg(1);
    u32 fid = j.arg(2);
    jvalue v{};
    if constexpr (IsStatic) {
        std::lock_guard<std::mutex> lk(g_meta_lock);
        auto it = g_static_fields.find(fid);
        if (it != g_static_fields.end()) v = it->second;
    } else {
        if (MockObject* o = get_object(target)) {
            auto it = o->fields.find(fid);
            if (it != o->fields.end()) v = it->second;
        }
    }

    if constexpr (T == 'J') {
        j.set_ret64(static_cast<u64>(v.j));
    } else if constexpr (T == 'D') {
        u64 b;
        std::memcpy(&b, &v.d, 8);
        j.set_ret64(b);
    } else if constexpr (T == 'F') {
        u32 b;
        std::memcpy(&b, &v.f, 4);
        j.set_ret(b);
    } else if constexpr (T == 'Z') {
        j.set_ret(v.z ? 1u : 0u);
    } else if constexpr (T == 'B') {
        j.set_ret(static_cast<u32>(static_cast<i32>(v.b)));
    } else if constexpr (T == 'C') {
        j.set_ret(static_cast<u32>(v.c));
    } else if constexpr (T == 'S') {
        j.set_ret(static_cast<u32>(static_cast<i32>(v.s)));
    } else if constexpr (T == 'I') {
        j.set_ret(static_cast<u32>(v.i));
    } else {
        j.set_ret(v.l);
    }
}

template <char T, bool IsStatic>
void set_field_thunk(JitCtx& j) {
    ArgReader r(j);
    r.w32(); // env
    u32 target = r.w32();
    u32 fid = r.w32();
    jvalue v{};
    if constexpr (T == 'J') {
        v.j = static_cast<jlong>(r.w64());
    } else if constexpr (T == 'D') {
        u64 b = r.w64();
        std::memcpy(&v.d, &b, 8);
    } else if constexpr (T == 'F') {
        u32 b = r.w32();
        std::memcpy(&v.f, &b, 4);
    } else if constexpr (T == 'Z') {
        v.z = static_cast<jboolean>(r.w32() & 0xFF);
    } else if constexpr (T == 'B') {
        v.b = static_cast<jbyte>(r.w32());
    } else if constexpr (T == 'C') {
        v.c = static_cast<jchar>(r.w32());
    } else if constexpr (T == 'S') {
        v.s = static_cast<jshort>(r.w32());
    } else if constexpr (T == 'I') {
        v.i = static_cast<jint>(r.w32());
    } else {
        v.l = r.w32();
    }

    if constexpr (IsStatic) {
        std::lock_guard<std::mutex> lk(g_meta_lock);
        g_static_fields[fid] = v;
    } else {
        if (MockObject* o = get_object(target)) {
            o->fields[fid] = v;
        }
    }
}

// -----------------------------------------------------------------------------
// Strings (Indices 163..170, 220..221, 224..225)
// -----------------------------------------------------------------------------
void thunk_NewString(JitCtx& j) {
    gaddr chars = j.arg(1);
    i32 len = static_cast<i32>(j.arg(2));
    std::string out;
    if (chars && len > 0) {
        const jchar* p = hp<const jchar>(chars);
        out.reserve(static_cast<size_t>(len));
        for (i32 i = 0; i < len; ++i) {
            out.push_back(static_cast<char>(p[i] & 0xFF));
        }
    }
    j.set_ret(create_local_string(out));
}

void thunk_GetStringLength(JitCtx& j) {
    const char* s = get_string_utf(j.arg(1));
    j.set_ret(s ? static_cast<u32>(std::strlen(s)) : 0);
}

void thunk_GetStringChars(JitCtx& j) {
    const char* s = get_string_utf(j.arg(1));
    gaddr is_copy = j.arg(2);
    size_t len = s ? std::strlen(s) : 0;
    gaddr buf = mem::gmalloc(static_cast<u32>((len + 1) * sizeof(jchar)));
    if (buf) {
        jchar* dst = hp<jchar>(buf);
        for (size_t i = 0; i < len; ++i) dst[i] = static_cast<u8>(s[i]);
        dst[len] = 0;
    }
    if (is_copy) wr<u8>(is_copy, 1);
    j.set_ret(buf);
}

void thunk_ReleaseStringChars(JitCtx& j) {
    gaddr chars = j.arg(2);
    if (chars) mem::gfree(chars);
}

void thunk_NewStringUTF(JitCtx& j) {
    const char* str = j.arg(1) ? hp<const char>(j.arg(1)) : "";
    j.set_ret(create_local_string(str ? str : ""));
}

void thunk_GetStringUTFLength(JitCtx& j) {
    const char* s = get_string_utf(j.arg(1));
    j.set_ret(s ? static_cast<u32>(std::strlen(s)) : 0);
}

void thunk_GetStringUTFChars(JitCtx& j) {
    u32 h = j.arg(1);
    gaddr is_copy = j.arg(2);
    const char* u = get_string_utf(h);
    gaddr gstr = u ? mem::gstrdup(u) : mem::gstrdup("");
    if (is_copy) wr<u8>(is_copy, 1);
    j.set_ret(gstr);
}

void thunk_ReleaseStringUTFChars(JitCtx& j) {
    gaddr ptr = j.arg(2);
    if (ptr) mem::gfree(ptr);
}

void thunk_GetStringRegion(JitCtx& j) {
    const char* s = get_string_utf(j.arg(1));
    i32 start = static_cast<i32>(j.arg(2));
    i32 len = static_cast<i32>(j.arg(3));
    gaddr buf = j.arg(4);
    if (!buf || len <= 0 || start < 0) return;
    size_t slen = s ? std::strlen(s) : 0;
    jchar* dst = hp<jchar>(buf);
    for (i32 i = 0; i < len; ++i) {
        size_t idx = static_cast<size_t>(start + i);
        dst[i] = (idx < slen) ? static_cast<u8>(s[idx]) : 0;
    }
}

void thunk_GetStringUTFRegion(JitCtx& j) {
    const char* s = get_string_utf(j.arg(1));
    i32 start = static_cast<i32>(j.arg(2));
    i32 len = static_cast<i32>(j.arg(3));
    gaddr buf = j.arg(4);
    if (!buf || len <= 0 || start < 0) return;
    size_t slen = s ? std::strlen(s) : 0;
    char* dst = hp<char>(buf);
    for (i32 i = 0; i < len; ++i) {
        size_t idx = static_cast<size_t>(start + i);
        dst[i] = (idx < slen) ? s[idx] : 0;
    }
}

// -----------------------------------------------------------------------------
// Arrays (Indices 171..214, 222..223)
// -----------------------------------------------------------------------------
void thunk_GetArrayLength(JitCtx& j) {
    u32 h = j.arg(1);
    MockObject* obj = get_object(h);
    if (!obj) {
        j.set_ret(0);
        return;
    }
    switch (obj->type) {
        case ObjectType::ByteArray:
        case ObjectType::BooleanArray:
            j.set_ret(static_cast<u32>(obj->byte_data.size()));
            break;
        case ObjectType::CharArray:
            j.set_ret(static_cast<u32>(obj->char_data.size()));
            break;
        case ObjectType::ShortArray:
            j.set_ret(static_cast<u32>(obj->short_data.size()));
            break;
        case ObjectType::IntArray:
            j.set_ret(static_cast<u32>(obj->int_data.size()));
            break;
        case ObjectType::LongArray:
            j.set_ret(static_cast<u32>(obj->long_data.size()));
            break;
        case ObjectType::FloatArray:
            j.set_ret(static_cast<u32>(obj->float_data.size()));
            break;
        case ObjectType::DoubleArray:
            j.set_ret(static_cast<u32>(obj->double_data.size()));
            break;
        case ObjectType::ObjectArray:
            j.set_ret(static_cast<u32>(obj->obj_data.size()));
            break;
        default:
            j.set_ret(0);
            break;
    }
}

void thunk_NewObjectArray(JitCtx& j) {
    i32 len = static_cast<i32>(j.arg(1));
    u32 init_elem = j.arg(3);
    u32 h = create_local_object(ObjectType::ObjectArray);
    if (MockObject* obj = get_object(h); obj && len > 0) {
        obj->obj_data.assign(static_cast<size_t>(len), init_elem);
    }
    j.set_ret(h);
}

void thunk_GetObjectArrayElement(JitCtx& j) {
    u32 h = j.arg(1);
    u32 idx = j.arg(2);
    MockObject* obj = get_object(h);
    if (obj && obj->type == ObjectType::ObjectArray && idx < obj->obj_data.size()) {
        j.set_ret(obj->obj_data[idx]);
    } else {
        j.set_ret(0);
    }
}

void thunk_SetObjectArrayElement(JitCtx& j) {
    u32 h = j.arg(1);
    u32 idx = j.arg(2);
    u32 val = j.arg(3);
    MockObject* obj = get_object(h);
    if (obj && obj->type == ObjectType::ObjectArray && idx < obj->obj_data.size()) {
        obj->obj_data[idx] = val;
    }
}

template <ObjectType ArrType, typename ElemT, auto VecMember>
struct PrimitiveArrayOps {
    static void NewArray(JitCtx& j) {
        i32 len = static_cast<i32>(j.arg(1));
        u32 h = create_local_object(ArrType);
        if (MockObject* obj = get_object(h); obj && len > 0) {
            (obj->*VecMember).resize(static_cast<size_t>(len), ElemT{});
        }
        j.set_ret(h);
    }

    static void GetElements(JitCtx& j) {
        u32 h = j.arg(1);
        gaddr is_copy = j.arg(2);
        MockObject* obj = get_object(h);
        size_t n = obj ? (obj->*VecMember).size() : 0;
        u32 bytes = static_cast<u32>((n > 0 ? n : 1) * sizeof(ElemT));
        gaddr buf = mem::gmalloc(bytes);
        if (buf && obj && n > 0) {
            std::memcpy(hp(buf), (obj->*VecMember).data(), n * sizeof(ElemT));
        } else if (buf) {
            std::memset(hp(buf), 0, bytes);
        }
        if (is_copy) wr<u8>(is_copy, 1);
        j.set_ret(buf);
    }

    static void ReleaseElements(JitCtx& j) {
        u32 h = j.arg(1);
        gaddr elems = j.arg(2);
        i32 mode = static_cast<i32>(j.arg(3));
        if (elems && mode != JNI_ABORT) {
            if (MockObject* obj = get_object(h)) {
                size_t n = (obj->*VecMember).size();
                if (n > 0) {
                    std::memcpy((obj->*VecMember).data(), hp(elems), n * sizeof(ElemT));
                }
            }
        }
        if (elems && mode != JNI_COMMIT) {
            mem::gfree(elems);
        }
    }

    static void GetRegion(JitCtx& j) {
        u32 h = j.arg(1);
        i32 start = static_cast<i32>(j.arg(2));
        i32 len = static_cast<i32>(j.arg(3));
        gaddr buf = j.arg(4);
        if (!buf || len <= 0 || start < 0) return;
        MockObject* obj = get_object(h);
        if (!obj) return;
        const auto& vec = obj->*VecMember;
        size_t s = static_cast<size_t>(start);
        size_t l = static_cast<size_t>(len);
        if (s < vec.size()) {
            size_t avail = std::min(l, vec.size() - s);
            std::memcpy(hp(buf), vec.data() + s, avail * sizeof(ElemT));
        }
    }

    static void SetRegion(JitCtx& j) {
        u32 h = j.arg(1);
        i32 start = static_cast<i32>(j.arg(2));
        i32 len = static_cast<i32>(j.arg(3));
        gaddr buf = j.arg(4);
        if (!buf || len <= 0 || start < 0) return;
        MockObject* obj = get_object(h);
        if (!obj) return;
        auto& vec = obj->*VecMember;
        size_t end_idx = static_cast<size_t>(start + len);
        if (vec.size() < end_idx) {
            vec.resize(end_idx, ElemT{});
        }
        std::memcpy(vec.data() + start, hp(buf), static_cast<size_t>(len) * sizeof(ElemT));
    }
};

using BoolArrayOps   = PrimitiveArrayOps<ObjectType::BooleanArray, u8,     &MockObject::byte_data>;
using ByteArrayOps   = PrimitiveArrayOps<ObjectType::ByteArray,    u8,     &MockObject::byte_data>;
using CharArrayOps   = PrimitiveArrayOps<ObjectType::CharArray,    u16,    &MockObject::char_data>;
using ShortArrayOps  = PrimitiveArrayOps<ObjectType::ShortArray,   i16,    &MockObject::short_data>;
using IntArrayOps    = PrimitiveArrayOps<ObjectType::IntArray,     u32,    &MockObject::int_data>;
using LongArrayOps   = PrimitiveArrayOps<ObjectType::LongArray,    i64,    &MockObject::long_data>;
using FloatArrayOps  = PrimitiveArrayOps<ObjectType::FloatArray,   float,  &MockObject::float_data>;
using DoubleArrayOps = PrimitiveArrayOps<ObjectType::DoubleArray,  double, &MockObject::double_data>;

void thunk_GetPrimitiveArrayCritical(JitCtx& j) {
    u32 h = j.arg(1);
    MockObject* obj = get_object(h);
    if (!obj) {
        j.set_ret(0);
        return;
    }
    switch (obj->type) {
        case ObjectType::BooleanArray: BoolArrayOps::GetElements(j); break;
        case ObjectType::ByteArray:    ByteArrayOps::GetElements(j); break;
        case ObjectType::CharArray:    CharArrayOps::GetElements(j); break;
        case ObjectType::ShortArray:   ShortArrayOps::GetElements(j); break;
        case ObjectType::IntArray:     IntArrayOps::GetElements(j); break;
        case ObjectType::LongArray:    LongArrayOps::GetElements(j); break;
        case ObjectType::FloatArray:   FloatArrayOps::GetElements(j); break;
        case ObjectType::DoubleArray:  DoubleArrayOps::GetElements(j); break;
        default: j.set_ret(0); break;
    }
}

void thunk_ReleasePrimitiveArrayCritical(JitCtx& j) {
    u32 h = j.arg(1);
    MockObject* obj = get_object(h);
    if (!obj) {
        gaddr elems = j.arg(2);
        if (elems) mem::gfree(elems);
        return;
    }
    switch (obj->type) {
        case ObjectType::BooleanArray: BoolArrayOps::ReleaseElements(j); break;
        case ObjectType::ByteArray:    ByteArrayOps::ReleaseElements(j); break;
        case ObjectType::CharArray:    CharArrayOps::ReleaseElements(j); break;
        case ObjectType::ShortArray:   ShortArrayOps::ReleaseElements(j); break;
        case ObjectType::IntArray:     IntArrayOps::ReleaseElements(j); break;
        case ObjectType::LongArray:    LongArrayOps::ReleaseElements(j); break;
        case ObjectType::FloatArray:   FloatArrayOps::ReleaseElements(j); break;
        case ObjectType::DoubleArray:  DoubleArrayOps::ReleaseElements(j); break;
        default: {
            gaddr elems = j.arg(2);
            if (elems) mem::gfree(elems);
            break;
        }
    }
}

// -----------------------------------------------------------------------------
// RegisterNatives, Monitors, JavaVM, DirectByteBuffer, WeakRefs (Indices 215..232)
// -----------------------------------------------------------------------------
void thunk_RegisterNatives(JitCtx& j) {
    u32 cls = j.arg(1);
    gaddr methods = j.arg(2);
    i32 n = static_cast<i32>(j.arg(3));
    MockObject* cls_obj = get_object(cls);
    const char* cname = (cls_obj && !cls_obj->class_name.empty()) ? cls_obj->class_name.c_str() : "unknown";
    for (i32 i = 0; i < n && methods; ++i) {
        gaddr name_ptr = rd<u32>(methods + 12 * i);
        gaddr sig_ptr  = rd<u32>(methods + 12 * i + 4);
        gaddr fn_ptr   = rd<u32>(methods + 12 * i + 8);
        const char* mname = name_ptr ? hp<const char>(name_ptr) : "";
        const char* msig  = sig_ptr ? hp<const char>(sig_ptr) : "";
        ZLOGD("RegisterNatives: %s.%s %s -> 0x%08x", cname, mname, msig, fn_ptr);
        if (std::strcmp(mname, "SetFilesDir") == 0) g_native_addrs.SetFilesDir = fn_ptr;
        else if (std::strcmp(mname, "SetUniqueDeviceID") == 0) g_native_addrs.SetUniqueDeviceID = fn_ptr;
        else if (std::strcmp(mname, "Poll") == 0) g_native_addrs.Poll = fn_ptr;
        else if (std::strcmp(mname, "Back") == 0) g_native_addrs.Back = fn_ptr;
        else if (std::strcmp(mname, "JNIEnterBackgroundState") == 0) g_native_addrs.JNIEnterBackgroundState = fn_ptr;
    }
    j.set_ret(JNI_OK);
}

void thunk_UnregisterNatives(JitCtx& j) {
    j.set_ret(JNI_OK);
}

void thunk_MonitorEnter(JitCtx& j) {
    j.set_ret(JNI_OK);
}

void thunk_MonitorExit(JitCtx& j) {
    j.set_ret(JNI_OK);
}

void thunk_GetJavaVM(JitCtx& j) {
    gaddr vm_out = j.arg(1);
    if (vm_out) wr<u32>(vm_out, g_guest_vm_ptr);
    j.set_ret(JNI_OK);
}

void thunk_NewWeakGlobalRef(JitCtx& j) {
    u32 src = j.arg(1);
    if (!src) {
        j.set_ret(0);
        return;
    }
    MockObject copy;
    if (MockObject* o = get_object(src)) copy = *o;
    u32 idx = alloc_slot(ObjectKind::Weak, copy.type, nullptr);
    {
        std::lock_guard<std::mutex> lk(g_slot_lock);
        g_slots[idx].obj = std::move(copy);
    }
    j.set_ret(handle_of(idx));
}

void thunk_DeleteWeakGlobalRef(JitCtx& j) {
    u32 h = j.arg(1);
    if (kind_of_handle(h) == ObjectKind::Weak) {
        delete_handle(h);
    }
}

void thunk_ExceptionCheck(JitCtx& j) {
    ensure_bias_from_caller(j.r()[14]);
    j.set_ret(t_pending_exception != 0 ? JNI_TRUE : JNI_FALSE);
}

void thunk_NewDirectByteBuffer(JitCtx& j) {
    ArgReader r(j);
    r.w32(); // env
    gaddr addr = r.w32();
    i64 cap = static_cast<i64>(r.w64());
    u32 h = create_local_object(ObjectType::DirectByteBuffer);
    if (MockObject* o = get_object(h)) {
        o->direct_addr = addr;
        o->direct_cap = cap;
    }
    j.set_ret(h);
}

void thunk_GetDirectBufferAddress(JitCtx& j) {
    MockObject* o = get_object(j.arg(1));
    j.set_ret(o ? o->direct_addr : 0);
}

void thunk_GetDirectBufferCapacity(JitCtx& j) {
    MockObject* o = get_object(j.arg(1));
    j.set_ret64(static_cast<u64>(o ? o->direct_cap : -1));
}

void thunk_GetObjectRefType(JitCtx& j) {
    switch (kind_of_handle(j.arg(1))) {
        case ObjectKind::Local:  j.set_ret(JNILocalRefType); break;
        case ObjectKind::Global: j.set_ret(JNIGlobalRefType); break;
        case ObjectKind::Weak:   j.set_ret(JNIWeakGlobalRefType); break;
        default:                 j.set_ret(JNIInvalidRefType); break;
    }
}

// -----------------------------------------------------------------------------
// JavaVM Invoke Interface Thunks
// -----------------------------------------------------------------------------
void thunk_JavaVM_AttachCurrentThread(JitCtx& j) {
    ensure_bias_from_caller(j.r()[14]);
    gaddr penv = j.arg(1);
    gaddr env = bind_env();
    if (penv) wr<u32>(penv, env);
    j.set_ret(JNI_OK);
}

void thunk_JavaVM_DetachCurrentThread(JitCtx& j) {
    for (auto& f : t_frames) release_frame(f);
    t_frames.clear();
    j.set_ret(JNI_OK);
}

void thunk_JavaVM_GetEnv(JitCtx& j) {
    ensure_bias_from_caller(j.r()[14]);
    gaddr penv = j.arg(1);
    gaddr env = bind_env();
    if (penv) wr<u32>(penv, env);
    j.set_ret(JNI_OK);
}

void thunk_JavaVM_DestroyJavaVM(JitCtx& j) {
    j.set_ret(JNI_ERR);
}

// Generic Stub with Exact Vtable Index Logging
void thunk_GenericStub(JitCtx& j) {
    size_t idx = 0;
    for (size_t i = 0; i < g_vtable_thunks.size(); ++i) {
        if (g_vtable_thunks[i] == j.cur_thunk) {
            idx = i;
            break;
        }
    }
    ZLOGW("Unimplemented JNI call through vtable (index %zu)", idx);
    j.set_ret(0);
}

// -----------------------------------------------------------------------------
// Table Construction (Full 233-entry JNINativeInterface Table)
// -----------------------------------------------------------------------------
void build_tables() {
    std::array<gaddr, 233> f{};
    auto make = [](ThunkFn fn, const char* name) { return cpu::make_thunk(fn, name); };

    // 4..33: Version, Classes, Exceptions, References, Object Creation, Method IDs
    f[4]  = make(&thunk_GetVersion, "JNI_GetVersion");
    f[5]  = make(&thunk_DefineClass, "JNI_DefineClass");
    f[6]  = make(&thunk_FindClass, "JNI_FindClass");
    f[7]  = make(&thunk_FromReflectedMethod, "JNI_FromReflectedMethod");
    f[8]  = make(&thunk_FromReflectedField, "JNI_FromReflectedField");
    f[9]  = make(&thunk_ToReflectedMethod, "JNI_ToReflectedMethod");
    f[10] = make(&thunk_GetSuperclass, "JNI_GetSuperclass");
    f[11] = make(&thunk_IsAssignableFrom, "JNI_IsAssignableFrom");
    f[12] = make(&thunk_ToReflectedField, "JNI_ToReflectedField");
    f[13] = make(&thunk_Throw, "JNI_Throw");
    f[14] = make(&thunk_ThrowNew, "JNI_ThrowNew");
    f[15] = make(&thunk_ExceptionOccurred, "JNI_ExceptionOccurred");
    f[16] = make(&thunk_ExceptionDescribe, "JNI_ExceptionDescribe");
    f[17] = make(&thunk_ExceptionClear, "JNI_ExceptionClear");
    f[18] = make(&thunk_FatalError, "JNI_FatalError");
    f[19] = make(&thunk_PushLocalFrame, "JNI_PushLocalFrame");
    f[20] = make(&thunk_PopLocalFrame, "JNI_PopLocalFrame");
    f[21] = make(&thunk_NewGlobalRef, "JNI_NewGlobalRef");
    f[22] = make(&thunk_DeleteGlobalRef, "JNI_DeleteGlobalRef");
    f[23] = make(&thunk_DeleteLocalRef, "JNI_DeleteLocalRef");
    f[24] = make(&thunk_IsSameObject, "JNI_IsSameObject");
    f[25] = make(&thunk_NewLocalRef, "JNI_NewLocalRef");
    f[26] = make(&thunk_EnsureLocalCapacity, "JNI_EnsureLocalCapacity");
    f[27] = make(&thunk_AllocObject, "JNI_AllocObject");
    f[28] = make(&new_object_thunk<kDots>, "JNI_NewObject");
    f[29] = make(&new_object_thunk<kVaList>, "JNI_NewObjectV");
    f[30] = make(&new_object_thunk<kArray>, "JNI_NewObjectA");
    f[31] = make(&thunk_GetObjectClass, "JNI_GetObjectClass");
    f[32] = make(&thunk_IsInstanceOf, "JNI_IsInstanceOf");
    f[33] = make(&thunk_GetMethodID, "JNI_GetMethodID");

#define ZB_REGISTER_CALLS(base, Kind, Prefix) \
    f[base + 0]  = make(&call_thunk<'L', Kind, kDots>,   "JNI_Call" Prefix "ObjectMethod"); \
    f[base + 1]  = make(&call_thunk<'L', Kind, kVaList>, "JNI_Call" Prefix "ObjectMethodV"); \
    f[base + 2]  = make(&call_thunk<'L', Kind, kArray>,  "JNI_Call" Prefix "ObjectMethodA"); \
    f[base + 3]  = make(&call_thunk<'Z', Kind, kDots>,   "JNI_Call" Prefix "BooleanMethod"); \
    f[base + 4]  = make(&call_thunk<'Z', Kind, kVaList>, "JNI_Call" Prefix "BooleanMethodV"); \
    f[base + 5]  = make(&call_thunk<'Z', Kind, kArray>,  "JNI_Call" Prefix "BooleanMethodA"); \
    f[base + 6]  = make(&call_thunk<'B', Kind, kDots>,   "JNI_Call" Prefix "ByteMethod"); \
    f[base + 7]  = make(&call_thunk<'B', Kind, kVaList>, "JNI_Call" Prefix "ByteMethodV"); \
    f[base + 8]  = make(&call_thunk<'B', Kind, kArray>,  "JNI_Call" Prefix "ByteMethodA"); \
    f[base + 9]  = make(&call_thunk<'C', Kind, kDots>,   "JNI_Call" Prefix "CharMethod"); \
    f[base + 10] = make(&call_thunk<'C', Kind, kVaList>, "JNI_Call" Prefix "CharMethodV"); \
    f[base + 11] = make(&call_thunk<'C', Kind, kArray>,  "JNI_Call" Prefix "CharMethodA"); \
    f[base + 12] = make(&call_thunk<'S', Kind, kDots>,   "JNI_Call" Prefix "ShortMethod"); \
    f[base + 13] = make(&call_thunk<'S', Kind, kVaList>, "JNI_Call" Prefix "ShortMethodV"); \
    f[base + 14] = make(&call_thunk<'S', Kind, kArray>,  "JNI_Call" Prefix "ShortMethodA"); \
    f[base + 15] = make(&call_thunk<'I', Kind, kDots>,   "JNI_Call" Prefix "IntMethod"); \
    f[base + 16] = make(&call_thunk<'I', Kind, kVaList>, "JNI_Call" Prefix "IntMethodV"); \
    f[base + 17] = make(&call_thunk<'I', Kind, kArray>,  "JNI_Call" Prefix "IntMethodA"); \
    f[base + 18] = make(&call_thunk<'J', Kind, kDots>,   "JNI_Call" Prefix "LongMethod"); \
    f[base + 19] = make(&call_thunk<'J', Kind, kVaList>, "JNI_Call" Prefix "LongMethodV"); \
    f[base + 20] = make(&call_thunk<'J', Kind, kArray>,  "JNI_Call" Prefix "LongMethodA"); \
    f[base + 21] = make(&call_thunk<'F', Kind, kDots>,   "JNI_Call" Prefix "FloatMethod"); \
    f[base + 22] = make(&call_thunk<'F', Kind, kVaList>, "JNI_Call" Prefix "FloatMethodV"); \
    f[base + 23] = make(&call_thunk<'F', Kind, kArray>,  "JNI_Call" Prefix "FloatMethodA"); \
    f[base + 24] = make(&call_thunk<'D', Kind, kDots>,   "JNI_Call" Prefix "DoubleMethod"); \
    f[base + 25] = make(&call_thunk<'D', Kind, kVaList>, "JNI_Call" Prefix "DoubleMethodV"); \
    f[base + 26] = make(&call_thunk<'D', Kind, kArray>,  "JNI_Call" Prefix "DoubleMethodA"); \
    f[base + 27] = make(&call_thunk<'V', Kind, kDots>,   "JNI_Call" Prefix "VoidMethod"); \
    f[base + 28] = make(&call_thunk<'V', Kind, kVaList>, "JNI_Call" Prefix "VoidMethodV"); \
    f[base + 29] = make(&call_thunk<'V', Kind, kArray>,  "JNI_Call" Prefix "VoidMethodA")

    // 34..63: Virtual Method Calls
    ZB_REGISTER_CALLS(34, kVirtual, "");
    // 64..93: Nonvirtual Method Calls
    ZB_REGISTER_CALLS(64, kNonvirtual, "Nonvirtual");

    // 94..112: Instance Fields
    f[94]  = make(&thunk_GetFieldID, "JNI_GetFieldID");
    f[95]  = make(&get_field_thunk<'L', false>, "JNI_GetObjectField");
    f[96]  = make(&get_field_thunk<'Z', false>, "JNI_GetBooleanField");
    f[97]  = make(&get_field_thunk<'B', false>, "JNI_GetByteField");
    f[98]  = make(&get_field_thunk<'C', false>, "JNI_GetCharField");
    f[99]  = make(&get_field_thunk<'S', false>, "JNI_GetShortField");
    f[100] = make(&get_field_thunk<'I', false>, "JNI_GetIntField");
    f[101] = make(&get_field_thunk<'J', false>, "JNI_GetLongField");
    f[102] = make(&get_field_thunk<'F', false>, "JNI_GetFloatField");
    f[103] = make(&get_field_thunk<'D', false>, "JNI_GetDoubleField");
    f[104] = make(&set_field_thunk<'L', false>, "JNI_SetObjectField");
    f[105] = make(&set_field_thunk<'Z', false>, "JNI_SetBooleanField");
    f[106] = make(&set_field_thunk<'B', false>, "JNI_SetByteField");
    f[107] = make(&set_field_thunk<'C', false>, "JNI_SetCharField");
    f[108] = make(&set_field_thunk<'S', false>, "JNI_SetShortField");
    f[109] = make(&set_field_thunk<'I', false>, "JNI_SetIntField");
    f[110] = make(&set_field_thunk<'J', false>, "JNI_SetLongField");
    f[111] = make(&set_field_thunk<'F', false>, "JNI_SetFloatField");
    f[112] = make(&set_field_thunk<'D', false>, "JNI_SetDoubleField");

    // 113..143: Static Method Calls
    f[113] = make(&thunk_GetStaticMethodID, "JNI_GetStaticMethodID");
    ZB_REGISTER_CALLS(114, kStatic, "Static");
#undef ZB_REGISTER_CALLS

    // 144..162: Static Fields
    f[144] = make(&thunk_GetStaticFieldID, "JNI_GetStaticFieldID");
    f[145] = make(&get_field_thunk<'L', true>, "JNI_GetStaticObjectField");
    f[146] = make(&get_field_thunk<'Z', true>, "JNI_GetStaticBooleanField");
    f[147] = make(&get_field_thunk<'B', true>, "JNI_GetStaticByteField");
    f[148] = make(&get_field_thunk<'C', true>, "JNI_GetStaticCharField");
    f[149] = make(&get_field_thunk<'S', true>, "JNI_GetStaticShortField");
    f[150] = make(&get_field_thunk<'I', true>, "JNI_GetStaticIntField");
    f[151] = make(&get_field_thunk<'J', true>, "JNI_GetStaticLongField");
    f[152] = make(&get_field_thunk<'F', true>, "JNI_GetStaticFloatField");
    f[153] = make(&get_field_thunk<'D', true>, "JNI_GetStaticDoubleField");
    f[154] = make(&set_field_thunk<'L', true>, "JNI_SetStaticObjectField");
    f[155] = make(&set_field_thunk<'Z', true>, "JNI_SetStaticBooleanField");
    f[156] = make(&set_field_thunk<'B', true>, "JNI_SetStaticByteField");
    f[157] = make(&set_field_thunk<'C', true>, "JNI_SetStaticCharField");
    f[158] = make(&set_field_thunk<'S', true>, "JNI_SetStaticShortField");
    f[159] = make(&set_field_thunk<'I', true>, "JNI_SetStaticIntField");
    f[160] = make(&set_field_thunk<'J', true>, "JNI_SetStaticLongField");
    f[161] = make(&set_field_thunk<'F', true>, "JNI_SetStaticFloatField");
    f[162] = make(&set_field_thunk<'D', true>, "JNI_SetStaticDoubleField");

    // 163..170: Strings
    f[163] = make(&thunk_NewString, "JNI_NewString");
    f[164] = make(&thunk_GetStringLength, "JNI_GetStringLength");
    f[165] = make(&thunk_GetStringChars, "JNI_GetStringChars");
    f[166] = make(&thunk_ReleaseStringChars, "JNI_ReleaseStringChars");
    f[167] = make(&thunk_NewStringUTF, "JNI_NewStringUTF");
    f[168] = make(&thunk_GetStringUTFLength, "JNI_GetStringUTFLength");
    f[169] = make(&thunk_GetStringUTFChars, "JNI_GetStringUTFChars");
    f[170] = make(&thunk_ReleaseStringUTFChars, "JNI_ReleaseStringUTFChars");

    // 171..214: Arrays
    f[171] = make(&thunk_GetArrayLength, "JNI_GetArrayLength");
    f[172] = make(&thunk_NewObjectArray, "JNI_NewObjectArray");
    f[173] = make(&thunk_GetObjectArrayElement, "JNI_GetObjectArrayElement");
    f[174] = make(&thunk_SetObjectArrayElement, "JNI_SetObjectArrayElement");

    f[175] = make(&BoolArrayOps::NewArray,   "JNI_NewBooleanArray");
    f[176] = make(&ByteArrayOps::NewArray,   "JNI_NewByteArray");
    f[177] = make(&CharArrayOps::NewArray,   "JNI_NewCharArray");
    f[178] = make(&ShortArrayOps::NewArray,  "JNI_NewShortArray");
    f[179] = make(&IntArrayOps::NewArray,    "JNI_NewIntArray");
    f[180] = make(&LongArrayOps::NewArray,   "JNI_NewLongArray");
    f[181] = make(&FloatArrayOps::NewArray,  "JNI_NewFloatArray");
    f[182] = make(&DoubleArrayOps::NewArray, "JNI_NewDoubleArray");

    f[183] = make(&BoolArrayOps::GetElements,   "JNI_GetBooleanArrayElements");
    f[184] = make(&ByteArrayOps::GetElements,   "JNI_GetByteArrayElements");
    f[185] = make(&CharArrayOps::GetElements,   "JNI_GetCharArrayElements");
    f[186] = make(&ShortArrayOps::GetElements,  "JNI_GetShortArrayElements");
    f[187] = make(&IntArrayOps::GetElements,    "JNI_GetIntArrayElements");
    f[188] = make(&LongArrayOps::GetElements,   "JNI_GetLongArrayElements");
    f[189] = make(&FloatArrayOps::GetElements,  "JNI_GetFloatArrayElements");
    f[190] = make(&DoubleArrayOps::GetElements, "JNI_GetDoubleArrayElements");

    f[191] = make(&BoolArrayOps::ReleaseElements,   "JNI_ReleaseBooleanArrayElements");
    f[192] = make(&ByteArrayOps::ReleaseElements,   "JNI_ReleaseByteArrayElements");
    f[193] = make(&CharArrayOps::ReleaseElements,   "JNI_ReleaseCharArrayElements");
    f[194] = make(&ShortArrayOps::ReleaseElements,  "JNI_ReleaseShortArrayElements");
    f[195] = make(&IntArrayOps::ReleaseElements,    "JNI_ReleaseIntArrayElements");
    f[196] = make(&LongArrayOps::ReleaseElements,   "JNI_ReleaseLongArrayElements");
    f[197] = make(&FloatArrayOps::ReleaseElements,  "JNI_ReleaseFloatArrayElements");
    f[198] = make(&DoubleArrayOps::ReleaseElements, "JNI_ReleaseDoubleArrayElements");

    f[199] = make(&BoolArrayOps::GetRegion,   "JNI_GetBooleanArrayRegion");
    f[200] = make(&ByteArrayOps::GetRegion,   "JNI_GetByteArrayRegion");
    f[201] = make(&CharArrayOps::GetRegion,   "JNI_GetCharArrayRegion");
    f[202] = make(&ShortArrayOps::GetRegion,  "JNI_GetShortArrayRegion");
    f[203] = make(&IntArrayOps::GetRegion,    "JNI_GetIntArrayRegion");
    f[204] = make(&LongArrayOps::GetRegion,   "JNI_GetLongArrayRegion");
    f[205] = make(&FloatArrayOps::GetRegion,  "JNI_GetFloatArrayRegion");
    f[206] = make(&DoubleArrayOps::GetRegion, "JNI_GetDoubleArrayRegion");

    f[207] = make(&BoolArrayOps::SetRegion,   "JNI_SetBooleanArrayRegion");
    f[208] = make(&ByteArrayOps::SetRegion,   "JNI_SetByteArrayRegion");
    f[209] = make(&CharArrayOps::SetRegion,   "JNI_SetCharArrayRegion");
    f[210] = make(&ShortArrayOps::SetRegion,  "JNI_SetShortArrayRegion");
    f[211] = make(&IntArrayOps::SetRegion,    "JNI_SetIntArrayRegion");
    f[212] = make(&LongArrayOps::SetRegion,   "JNI_SetLongArrayRegion");
    f[213] = make(&FloatArrayOps::SetRegion,  "JNI_SetFloatArrayRegion");
    f[214] = make(&DoubleArrayOps::SetRegion, "JNI_SetDoubleArrayRegion");

    // 215..232: Natives, Monitors, JavaVM, Critical, DirectBuffer, RefType
    f[215] = make(&thunk_RegisterNatives, "JNI_RegisterNatives");
    f[216] = make(&thunk_UnregisterNatives, "JNI_UnregisterNatives");
    f[217] = make(&thunk_MonitorEnter, "JNI_MonitorEnter");
    f[218] = make(&thunk_MonitorExit, "JNI_MonitorExit");
    f[219] = make(&thunk_GetJavaVM, "JNI_GetJavaVM");
    f[220] = make(&thunk_GetStringRegion, "JNI_GetStringRegion");
    f[221] = make(&thunk_GetStringUTFRegion, "JNI_GetStringUTFRegion");
    f[222] = make(&thunk_GetPrimitiveArrayCritical, "JNI_GetPrimitiveArrayCritical");
    f[223] = make(&thunk_ReleasePrimitiveArrayCritical, "JNI_ReleasePrimitiveArrayCritical");
    f[224] = make(&thunk_GetStringChars, "JNI_GetStringCritical");
    f[225] = make(&thunk_ReleaseStringChars, "JNI_ReleaseStringCritical");
    f[226] = make(&thunk_NewWeakGlobalRef, "JNI_NewWeakGlobalRef");
    f[227] = make(&thunk_DeleteWeakGlobalRef, "JNI_DeleteWeakGlobalRef");
    f[228] = make(&thunk_ExceptionCheck, "JNI_ExceptionCheck");
    f[229] = make(&thunk_NewDirectByteBuffer, "JNI_NewDirectByteBuffer");
    f[230] = make(&thunk_GetDirectBufferAddress, "JNI_GetDirectBufferAddress");
    f[231] = make(&thunk_GetDirectBufferCapacity, "JNI_GetDirectBufferCapacity");
    f[232] = make(&thunk_GetObjectRefType, "JNI_GetObjectRefType");

    // Fill any unpopulated slot with an index-aware generic stub
    for (size_t i = 0; i < f.size(); ++i) {
        if (!f[i]) {
            char stub_name[32];
            std::snprintf(stub_name, sizeof(stub_name), "JNI_GenericStub_%zu", i);
            f[i] = make(&thunk_GenericStub, stub_name);
        }
        g_vtable_thunks[i] = f[i];
    }

    g_guest_env_table = mem::gmalloc(static_cast<u32>(f.size() * 4));
    for (size_t i = 0; i < f.size(); ++i) {
        wr<u32>(g_guest_env_table + static_cast<u32>(i * 4), f[i]);
    }

    // JavaVM table
    gaddr inv = mem::gcalloc(8, 4);
    wr<u32>(inv + 12, make(&thunk_JavaVM_DestroyJavaVM, "JavaVM_DestroyJavaVM"));
    wr<u32>(inv + 16, make(&thunk_JavaVM_AttachCurrentThread, "JavaVM_AttachCurrentThread"));
    wr<u32>(inv + 20, make(&thunk_JavaVM_DetachCurrentThread, "JavaVM_DetachCurrentThread"));
    wr<u32>(inv + 24, make(&thunk_JavaVM_GetEnv, "JavaVM_GetEnv"));
    wr<u32>(inv + 28, make(&thunk_JavaVM_AttachCurrentThread, "JavaVM_AttachCurrentThreadAsDaemon"));

    g_guest_vm_ptr = mem::gmalloc(4);
    wr<u32>(g_guest_vm_ptr, inv);

    // Main thread guest env pointer
    g_main_guest_env = mem::gmalloc(4);
    wr<u32>(g_main_guest_env, g_guest_env_table);
}

} // namespace

// -----------------------------------------------------------------------------
// Public Subsystem API
// -----------------------------------------------------------------------------
void init() {
    static std::once_flag once;
    std::call_once(once, [] {
        build_tables();
        ZLOGI("Mock JavaVM & JNIEnv initialized (VM: 0x%08x, Env: 0x%08x)", g_guest_vm_ptr, g_main_guest_env);
    });
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_slot_lock);
    g_slots.clear();
    g_free_head = UINT32_MAX;
}

gaddr guest_vm() {
    return g_guest_vm_ptr;
}

gaddr current_env() {
    GuestThread* t = cpu::current_thread_if_any();
    if (t && t->guest_env) return t->guest_env;
    return g_main_guest_env;
}

gaddr bind_env() {
    GuestThread* t = cpu::current_thread();
    if (!t->guest_env) {
        t->guest_env = t->tls + tls::kJniEnv;
        wr<u32>(t->guest_env, g_guest_env_table);
    }
    return t->guest_env;
}

u32 create_local_object(ObjectType type) {
    u32 gen = 0;
    u32 idx = alloc_slot(ObjectKind::Local, type, &gen);
    current_frame().emplace_back(idx, gen);
    return handle_of(idx);
}

u32 create_local_string(const std::string& str) {
    u32 h = create_local_object(ObjectType::String);
    MockObject* obj = get_object(h);
    if (obj) {
        obj->str_val = str;
        obj->class_name = "java/lang/String";
    }
    return h;
}

u32 create_local_byte_array(const void* data, size_t size) {
    u32 h = create_local_object(ObjectType::ByteArray);
    MockObject* obj = get_object(h);
    if (obj && data && size) {
        obj->byte_data.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
    }
    return h;
}

u32 create_local_int_array(const int32_t* data, size_t count) {
    u32 h = create_local_object(ObjectType::IntArray);
    MockObject* obj = get_object(h);
    if (obj && data && count) {
        obj->int_data.assign(data, data + count);
    }
    return h;
}

u32 create_local_class(const std::string& class_name) {
    u32 h = create_local_object(ObjectType::Class);
    MockObject* obj = get_object(h);
    if (obj) obj->class_name = class_name;
    return h;
}

u32 to_global_handle(u32 local_handle) {
    u32 idx = 0;
    if (!idx_of(local_handle, &idx)) return 0;
    std::lock_guard<std::mutex> lk(g_slot_lock);
    if (idx >= g_slots.size() || g_slots[idx].kind == ObjectKind::Free) return 0;
    g_slots[idx].kind = ObjectKind::Global;
    return local_handle;
}

void delete_handle(u32 handle) {
    u32 idx = 0;
    if (!idx_of(handle, &idx)) return;
    std::lock_guard<std::mutex> lk(g_slot_lock);
    if (idx < g_slots.size() && g_slots[idx].kind != ObjectKind::Free) {
        free_slot_locked(idx);
    }
}

MockObject* get_object(u32 handle) {
    u32 idx = 0;
    if (!idx_of(handle, &idx)) return nullptr;
    std::lock_guard<std::mutex> lk(g_slot_lock);
    if (idx >= g_slots.size() || g_slots[idx].kind == ObjectKind::Free) return nullptr;
    return &g_slots[idx].obj;
}

const char* get_string_utf(u32 handle) {
    MockObject* obj = get_object(handle);
    return (obj && obj->type == ObjectType::String) ? obj->str_val.c_str() : "";
}

LocalFrameScope::LocalFrameScope() {
    t_frames.emplace_back();
}

LocalFrameScope::~LocalFrameScope() {
    if (!t_frames.empty()) {
        release_frame(t_frames.back());
        t_frames.pop_back();
    }
}

// -----------------------------------------------------------------------------
// 18 JNI Native Methods Wrappers
// -----------------------------------------------------------------------------
void set_native_addresses(const NativeAddresses& addrs) {
    g_native_addrs = addrs;
    g_bias_resolved = (addrs.SetFilesDir > 0x01000000u);
}

const NativeAddresses& get_native_addresses() {
    return g_native_addrs;
}

void resolve_native_addresses(gaddr module_bias) {
    NativeAddresses base;
    g_native_addrs.SetFilesDir                 = module_bias + base.SetFilesDir;
    g_native_addrs.SetUniqueDeviceID           = module_bias + base.SetUniqueDeviceID;
    g_native_addrs.Poll                        = module_bias + base.Poll;
    g_native_addrs.Back                        = module_bias + base.Back;
    g_native_addrs.JNIEnterBackgroundState     = module_bias + base.JNIEnterBackgroundState;
    g_native_addrs.NativeExplicitAwardPurchase = module_bias + base.NativeExplicitAwardPurchase;
    g_native_addrs.OwnsAutoShield              = module_bias + base.OwnsAutoShield;
    g_native_addrs.OwnsDoubleBubble            = module_bias + base.OwnsDoubleBubble;
    g_native_addrs.OwnsFreeBoosts              = module_bias + base.OwnsFreeBoosts;
    g_native_addrs.OwnsMoreGoodies             = module_bias + base.OwnsMoreGoodies;
    g_native_addrs.OwnsSantaCostume            = module_bias + base.OwnsSantaCostume;
    g_native_addrs.RestoreAutoShield           = module_bias + base.RestoreAutoShield;
    g_native_addrs.RestoreDoubleBubble         = module_bias + base.RestoreDoubleBubble;
    g_native_addrs.RestoreFreeBoost            = module_bias + base.RestoreFreeBoost;
    g_native_addrs.RestoreMoreGoodies          = module_bias + base.RestoreMoreGoodies;
    g_native_addrs.RestoreSantaCostume         = module_bias + base.RestoreSantaCostume;
    g_native_addrs.sceConvert                  = module_bias + base.sceConvert;
    g_native_addrs.sceInvert                   = module_bias + base.sceInvert;
    g_bias_resolved = (module_bias != 0);
    ZLOGI("Resolved 18 libRSBR JNI native addresses with bias 0x%08x (SetFilesDir=0x%08x, Poll=0x%08x)",
          module_bias, g_native_addrs.SetFilesDir, g_native_addrs.Poll);
}

void call_SetFilesDir(const std::string& internal_dir, const std::string& external_dir) {
    LocalFrameScope scope;
    u32 s1 = create_local_string(internal_dir);
    u32 s2 = create_local_string(external_dir);
    gaddr env = current_env();
    cpu::call(g_native_addrs.SetFilesDir, cpu::Args().u32v(env).u32v(0).u32v(s1).u32v(s2));
    ZLOGI("SetFilesDir configured to '%s'", internal_dir.c_str());
}

void call_SetUniqueDeviceID(const std::string& device_id) {
    LocalFrameScope scope;
    u32 s = create_local_string(device_id);
    gaddr env = current_env();
    cpu::call(g_native_addrs.SetUniqueDeviceID, cpu::Args().u32v(env).u32v(0).u32v(s));
    ZLOGI("SetUniqueDeviceID configured to '%s'", device_id.c_str());
}

void call_Poll() {
    LocalFrameScope scope;
    u32 self = create_local_class("com/playstation/runsackboyrun/JNI");
    gaddr env = current_env();
    cpu::call(g_native_addrs.Poll, cpu::Args().u32v(env).u32v(self));
}

bool call_Back() {
    LocalFrameScope scope;
    gaddr env = current_env();
    u64 res = cpu::call(g_native_addrs.Back, cpu::Args().u32v(env).u32v(0));
    return (res & 0xFF) != 0;
}

void call_JNIEnterBackgroundState() {
    LocalFrameScope scope;
    gaddr env = current_env();
    cpu::call(g_native_addrs.JNIEnterBackgroundState, cpu::Args().u32v(env).u32v(0));
}

void call_NativeExplicitAwardPurchase(const std::string& sku) {
    LocalFrameScope scope;
    u32 s = create_local_string(sku);
    gaddr env = current_env();
    cpu::call(g_native_addrs.NativeExplicitAwardPurchase, cpu::Args().u32v(env).u32v(0).u32v(s));
    ZLOGI("Awarded purchase for SKU: %s", sku.c_str());
}

bool call_OwnsAutoShield() {
    gaddr env = current_env();
    return (cpu::call(g_native_addrs.OwnsAutoShield, cpu::Args().u32v(env).u32v(0)) & 0xFF) != 0;
}

bool call_OwnsDoubleBubble() {
    gaddr env = current_env();
    return (cpu::call(g_native_addrs.OwnsDoubleBubble, cpu::Args().u32v(env).u32v(0)) & 0xFF) != 0;
}

bool call_OwnsFreeBoosts() {
    gaddr env = current_env();
    return (cpu::call(g_native_addrs.OwnsFreeBoosts, cpu::Args().u32v(env).u32v(0)) & 0xFF) != 0;
}

bool call_OwnsMoreGoodies() {
    gaddr env = current_env();
    return (cpu::call(g_native_addrs.OwnsMoreGoodies, cpu::Args().u32v(env).u32v(0)) & 0xFF) != 0;
}

bool call_OwnsSantaCostume() {
    gaddr env = current_env();
    return (cpu::call(g_native_addrs.OwnsSantaCostume, cpu::Args().u32v(env).u32v(0)) & 0xFF) != 0;
}

void call_RestoreAutoShield() {
    gaddr env = current_env();
    cpu::call(g_native_addrs.RestoreAutoShield, cpu::Args().u32v(env).u32v(0));
}

void call_RestoreDoubleBubble() {
    gaddr env = current_env();
    cpu::call(g_native_addrs.RestoreDoubleBubble, cpu::Args().u32v(env).u32v(0));
}

void call_RestoreFreeBoost() {
    gaddr env = current_env();
    cpu::call(g_native_addrs.RestoreFreeBoost, cpu::Args().u32v(env).u32v(0));
}

void call_RestoreMoreGoodies() {
    gaddr env = current_env();
    cpu::call(g_native_addrs.RestoreMoreGoodies, cpu::Args().u32v(env).u32v(0));
}

void call_RestoreSantaCostume() {
    gaddr env = current_env();
    cpu::call(g_native_addrs.RestoreSantaCostume, cpu::Args().u32v(env).u32v(0));
}

std::string call_sceConvert(const std::string& token) {
    LocalFrameScope scope;
    u32 s = create_local_string(token);
    gaddr env = current_env();
    u32 res = static_cast<u32>(cpu::call(g_native_addrs.sceConvert, cpu::Args().u32v(env).u32v(0).u32v(s)));
    return get_string_utf(res);
}

std::string call_sceInvert(const std::string& token) {
    LocalFrameScope scope;
    u32 s = create_local_string(token);
    gaddr env = current_env();
    u32 res = static_cast<u32>(cpu::call(g_native_addrs.sceInvert, cpu::Args().u32v(env).u32v(0).u32v(s)));
    return get_string_utf(res);
}

void set_offline_config(const OfflineConfig& cfg) {
    g_offline_config = cfg;
}

const OfflineConfig& get_offline_config() {
    return g_offline_config;
}

} // namespace zb::jni
