#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

#include "core/common.h"
#include "core/cpu.h"
#include "core/mem_win32.h"

namespace zb::jni {

// Standard JNI 1.6 primitive types
using jboolean = uint8_t;
using jbyte    = int8_t;
using jchar    = uint16_t;
using jshort   = int16_t;
using jint     = int32_t;
using jlong    = int64_t;
using jfloat   = float;
using jdouble  = double;
using jsize    = int32_t;

// Guest 32-bit opaque handle types
using jobject        = u32;
using jclass         = jobject;
using jstring        = jobject;
using jarray         = jobject;
using jobjectArray   = jarray;
using jbooleanArray  = jarray;
using jbyteArray     = jarray;
using jcharArray     = jarray;
using jshortArray    = jarray;
using jintArray      = jarray;
using jlongArray     = jarray;
using jfloatArray    = jarray;
using jdoubleArray   = jarray;
using jthrowable     = jobject;
using jweak          = jobject;
using jmethodID      = u32;
using jfieldID       = u32;

constexpr jboolean JNI_FALSE = 0;
constexpr jboolean JNI_TRUE  = 1;

constexpr jint JNI_OK        = 0;
constexpr jint JNI_ERR       = (-1);
constexpr jint JNI_EDETACHED = (-2);
constexpr jint JNI_EVERSION  = (-3);
constexpr jint JNI_VERSION_1_6 = 0x00010006;

constexpr jint JNI_COMMIT    = 1;
constexpr jint JNI_ABORT     = 2;

enum jobjectRefType {
    JNIInvalidRefType    = 0,
    JNILocalRefType      = 1,
    JNIGlobalRefType     = 2,
    JNIWeakGlobalRefType = 3
};

union jvalue {
    jboolean z;
    jbyte    b;
    jchar    c;
    jshort   s;
    jint     i;
    jlong    j;
    jfloat   f;
    jdouble  d;
    jobject  l;
};

// -----------------------------------------------------------------------------
// Host Mock Object System
// -----------------------------------------------------------------------------
enum class ObjectKind : u8 {
    Free,
    Local,
    Global,
    Weak
};

enum class ObjectType : u8 {
    Generic,
    Class,
    String,
    ByteArray,
    IntArray,
    ObjectArray,
    BooleanArray,
    CharArray,
    ShortArray,
    LongArray,
    FloatArray,
    DoubleArray,
    DirectByteBuffer,
    Throwable
};

struct MockObject {
    ObjectType type = ObjectType::Generic;
    std::string str_val;
    std::vector<u8> byte_data;
    std::vector<u32> int_data;
    std::vector<u32> obj_data;
    std::string class_name;
    std::vector<u16> char_data;
    std::vector<i16> short_data;
    std::vector<i64> long_data;
    std::vector<float> float_data;
    std::vector<double> double_data;
    gaddr direct_addr = 0;
    i64 direct_cap = 0;
    std::unordered_map<u32, jvalue> fields;
};

// -----------------------------------------------------------------------------
// JNI Method Registry & Mock Dispatch
// -----------------------------------------------------------------------------
struct MethodSignature {
    std::string class_name;
    std::string method_name;
    std::string signature;
    std::string arg_types; // One char per arg: Z, B, C, S, I, J, F, D, L
    char return_type = 'V';
};

using MockMethodHandler = std::function<u64(const MethodSignature&, const std::vector<jvalue>&)>;

// -----------------------------------------------------------------------------
// Subsystem Initialization & Lifecycle
// -----------------------------------------------------------------------------
void init();
void shutdown();

// Guest pointers to JavaVM and JNIEnv tables
gaddr guest_vm();
gaddr bind_env();
gaddr current_env();

// Object handle allocation & management
u32 create_local_object(ObjectType type = ObjectType::Generic);
u32 create_local_string(const std::string& str);
u32 create_local_byte_array(const void* data, size_t size);
u32 create_local_int_array(const int32_t* data, size_t count);
u32 create_local_class(const std::string& class_name);
u32 to_global_handle(u32 local_handle);
void delete_handle(u32 handle);

MockObject* get_object(u32 handle);
const char* get_string_utf(u32 handle);

// Frame management for Java -> Guest call scoping
class LocalFrameScope {
public:
    LocalFrameScope();
    ~LocalFrameScope();
    LocalFrameScope(const LocalFrameScope&) = delete;
    LocalFrameScope& operator=(const LocalFrameScope&) = delete;
};

// -----------------------------------------------------------------------------
// 18 JNI Native Methods Interface (com.playstation.runsackboyrun.JNI & PSN)
// -----------------------------------------------------------------------------
struct NativeAddresses {
    gaddr SetFilesDir               = 0x001b1c60;
    gaddr SetUniqueDeviceID         = 0x001b1cec;
    gaddr Poll                      = 0x001b1dec;
    gaddr Back                      = 0x001b1de8;
    gaddr JNIEnterBackgroundState   = 0x001b1c50;
    gaddr NativeExplicitAwardPurchase = 0x001b1c08;
    gaddr OwnsAutoShield            = 0x001b1de0;
    gaddr OwnsDoubleBubble          = 0x001b1dd4;
    gaddr OwnsFreeBoosts            = 0x001b1dd8;
    gaddr OwnsMoreGoodies           = 0x001b1ddc;
    gaddr OwnsSantaCostume          = 0x001b1de4;
    gaddr RestoreAutoShield         = 0x001b1d94;
    gaddr RestoreDoubleBubble       = 0x001b1d34;
    gaddr RestoreFreeBoost          = 0x001b1d74;
    gaddr RestoreMoreGoodies        = 0x001b1d54;
    gaddr RestoreSantaCostume       = 0x001b1db4;
    gaddr sceConvert                = 0x001c5ec0;
    gaddr sceInvert                 = 0x001c60d4;
};

void set_native_addresses(const NativeAddresses& addrs);
const NativeAddresses& get_native_addresses();
void resolve_native_addresses(gaddr module_bias);

// High-level Host wrappers calling guest JNI exports:
void call_SetFilesDir(const std::string& internal_dir, const std::string& external_dir);
void call_SetUniqueDeviceID(const std::string& device_id);
void call_Poll();
bool call_Back();
void call_JNIEnterBackgroundState();
void call_NativeExplicitAwardPurchase(const std::string& sku);
bool call_OwnsAutoShield();
bool call_OwnsDoubleBubble();
bool call_OwnsFreeBoosts();
bool call_OwnsMoreGoodies();
bool call_OwnsSantaCostume();
void call_RestoreAutoShield();
void call_RestoreDoubleBubble();
void call_RestoreFreeBoost();
void call_RestoreMoreGoodies();
void call_RestoreSantaCostume();
std::string call_sceConvert(const std::string& token);
std::string call_sceInvert(const std::string& token);

// -----------------------------------------------------------------------------
// Configuration & Offline Mode Settings
// -----------------------------------------------------------------------------
struct OfflineConfig {
    bool unlock_all_iap = true;          // Automatically grant all IAP perks
    bool psn_offline_mode = true;        // Report PSN cleanly as offline
    bool facebook_offline_mode = true;   // Report Facebook cleanly as offline
    bool enable_telemetry_stubs = true;  // Safe silent no-op for analytics
    float display_density = 1.0f;        // Virtual desktop DPI scale
    std::string device_id = "sbr-pc-00000000-0000-0000-0000-000000000001";
    std::string language = "en";  // device locale as Android reports it (ISO 639-1)
    std::string country = "US";   // ISO 3166-1 alpha-2
    std::string save_directory = ".\\save\\";
};

void set_offline_config(const OfflineConfig& cfg);
const OfflineConfig& get_offline_config();

} // namespace zb::jni
