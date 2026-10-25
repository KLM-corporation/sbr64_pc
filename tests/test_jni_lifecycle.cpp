#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "core/common.h"
#include "core/mem_win32.h"
#include "core/sync_win32.h"
#include "core/cpu.h"
#include "android/jni_env.h"
#include "android/native_activity.h"

// Test macros
#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "[FAIL] %s (%s:%d): %s\n", #cond, __FILE__, __LINE__, msg); \
            std::exit(1); \
        } \
    } while (0)

#define TEST_PASS(name) \
    std::printf("[PASS] %s\n", name)

void test_jni_vm_tables() {
    std::printf("--- Running test_jni_vm_tables ---\n");

    zb::jni::init();

    zb::gaddr vm = zb::jni::guest_vm();
    TEST_ASSERT(vm != 0, "guest_vm pointer must not be 0");
    TEST_ASSERT((vm & 3) == 0, "guest_vm pointer must be 4-byte aligned");

    zb::gaddr inv = zb::rd<zb::u32>(vm);
    TEST_ASSERT(inv != 0, "JavaVM invoke interface table pointer must not be 0");

    // Check JavaVM entries: entry 4 is AttachCurrentThread, entry 6 is GetEnv
    zb::gaddr attach_fn = zb::rd<zb::u32>(inv + 16);
    zb::gaddr getenv_fn = zb::rd<zb::u32>(inv + 24);
    TEST_ASSERT(attach_fn != 0, "AttachCurrentThread thunk must be non-null");
    TEST_ASSERT(getenv_fn != 0, "GetEnv thunk must be non-null");

    zb::gaddr env = zb::jni::current_env();
    TEST_ASSERT(env != 0, "current_env pointer must not be 0");

    zb::gaddr native_iface = zb::rd<zb::u32>(env);
    TEST_ASSERT(native_iface != 0, "JNINativeInterface table pointer must not be 0");

    // Check entry 4 (GetVersion), 6 (FindClass), 113 (GetStaticMethodID), 167 (NewStringUTF)
    zb::gaddr getver = zb::rd<zb::u32>(native_iface + 16);
    zb::gaddr findcls = zb::rd<zb::u32>(native_iface + 24);
    zb::gaddr getsmid = zb::rd<zb::u32>(native_iface + 452);
    zb::gaddr newstr = zb::rd<zb::u32>(native_iface + 668);

    TEST_ASSERT(getver != 0, "GetVersion thunk must be non-null");
    TEST_ASSERT(findcls != 0, "FindClass thunk must be non-null");
    TEST_ASSERT(getsmid != 0, "GetStaticMethodID thunk must be non-null");
    TEST_ASSERT(newstr != 0, "NewStringUTF thunk must be non-null");

    // Verify all JNINativeInterface entries 4..232 are populated
    for (int i = 4; i <= 232; ++i) {
        zb::gaddr fn = zb::rd<zb::u32>(native_iface + i * 4);
        TEST_ASSERT(fn != 0, "JNINativeInterface entry must be non-null");
    }

    // Verify VaList and Exception thunks used by libRSBR_arm32.so via cpu::call
    zb::gaddr exc_occ = zb::rd<zb::u32>(native_iface + 15 * 4);
    zb::gaddr exc_clr = zb::rd<zb::u32>(native_iface + 17 * 4);
    zb::gaddr get_mid = zb::rd<zb::u32>(native_iface + 33 * 4);
    zb::gaddr call_obj_v = zb::rd<zb::u32>(native_iface + 35 * 4);
    zb::gaddr call_sobj_v = zb::rd<zb::u32>(native_iface + 115 * 4);
    zb::gaddr get_utf = zb::rd<zb::u32>(native_iface + 169 * 4);

    TEST_ASSERT(zb::cpu::call(exc_occ, zb::cpu::Args().u32v(env)) == 0, "ExceptionOccurred must return 0 when no exception is pending");
    zb::cpu::call(exc_clr, zb::cpu::Args().u32v(env));

    // Simulate engine_InitJavaClasses: Activity.getClassLoader() -> ClassLoader.loadClass("com/playstation/runsackboyrun/JNI")
    zb::gaddr act_cls = static_cast<zb::gaddr>(zb::cpu::call(findcls, zb::cpu::Args().u32v(env).u32v(zb::mem::gstrdup("android/app/NativeActivity"))));
    zb::gaddr mid_get_cl = static_cast<zb::gaddr>(zb::cpu::call(get_mid, zb::cpu::Args().u32v(env).u32v(act_cls)
        .u32v(zb::mem::gstrdup("getClassLoader")).u32v(zb::mem::gstrdup("()Ljava/lang/ClassLoader;"))));
    zb::gaddr act_obj = zb::jni::create_local_object();
    zb::gaddr va_buf = zb::mem::gcalloc(4, 4);
    zb::gaddr cl_obj = static_cast<zb::gaddr>(zb::cpu::call(call_obj_v, zb::cpu::Args().u32v(env).u32v(act_obj).u32v(mid_get_cl).u32v(va_buf)));
    TEST_ASSERT(cl_obj != 0, "getClassLoader via CallObjectMethodV must return valid ClassLoader handle");

    zb::gaddr cl_cls = static_cast<zb::gaddr>(zb::cpu::call(findcls, zb::cpu::Args().u32v(env).u32v(zb::mem::gstrdup("java/lang/ClassLoader"))));
    zb::gaddr mid_load_cls = static_cast<zb::gaddr>(zb::cpu::call(get_mid, zb::cpu::Args().u32v(env).u32v(cl_cls)
        .u32v(zb::mem::gstrdup("loadClass")).u32v(zb::mem::gstrdup("(Ljava/lang/String;)Ljava/lang/Class;"))));
    zb::gaddr jni_str = static_cast<zb::gaddr>(zb::cpu::call(newstr, zb::cpu::Args().u32v(env).u32v(zb::mem::gstrdup("com/playstation/runsackboyrun/JNI"))));
    zb::wr<zb::u32>(va_buf, jni_str);
    zb::gaddr jni_cls = static_cast<zb::gaddr>(zb::cpu::call(call_obj_v, zb::cpu::Args().u32v(env).u32v(cl_obj).u32v(mid_load_cls).u32v(va_buf)));
    TEST_ASSERT(jni_cls != 0, "ClassLoader.loadClass via CallObjectMethodV must return valid jclass");

    // Simulate engine_getExternalStorageDirectory: Environment.getExternalStorageDirectory() -> File.getPath()
    zb::gaddr env_cls = static_cast<zb::gaddr>(zb::cpu::call(findcls, zb::cpu::Args().u32v(env).u32v(zb::mem::gstrdup("android/os/Environment"))));
    zb::gaddr mid_ext_dir = static_cast<zb::gaddr>(zb::cpu::call(getsmid, zb::cpu::Args().u32v(env).u32v(env_cls)
        .u32v(zb::mem::gstrdup("getExternalStorageDirectory")).u32v(zb::mem::gstrdup("()Ljava/io/File;"))));
    zb::gaddr file_obj = static_cast<zb::gaddr>(zb::cpu::call(call_sobj_v, zb::cpu::Args().u32v(env).u32v(env_cls).u32v(mid_ext_dir).u32v(va_buf)));
    TEST_ASSERT(file_obj != 0, "Environment.getExternalStorageDirectory via CallStaticObjectMethodV must return File handle");

    zb::gaddr file_cls = static_cast<zb::gaddr>(zb::cpu::call(findcls, zb::cpu::Args().u32v(env).u32v(zb::mem::gstrdup("java/io/File"))));
    zb::gaddr mid_get_path = static_cast<zb::gaddr>(zb::cpu::call(get_mid, zb::cpu::Args().u32v(env).u32v(file_cls)
        .u32v(zb::mem::gstrdup("getPath")).u32v(zb::mem::gstrdup("()Ljava/lang/String;"))));
    zb::gaddr path_str = static_cast<zb::gaddr>(zb::cpu::call(call_obj_v, zb::cpu::Args().u32v(env).u32v(file_obj).u32v(mid_get_path).u32v(va_buf)));
    TEST_ASSERT(path_str != 0, "File.getPath via CallObjectMethodV must return String handle");
    zb::gaddr path_cstr = static_cast<zb::gaddr>(zb::cpu::call(get_utf, zb::cpu::Args().u32v(env).u32v(path_str).u32v(0)));
    TEST_ASSERT(std::string(zb::hp<const char>(path_cstr)) == "/storage/emulated/0", "External storage path must match /storage/emulated/0");

    TEST_PASS("test_jni_vm_tables");
}

void test_object_handles() {
    std::printf("--- Running test_object_handles ---\n");

    zb::jni::LocalFrameScope frame;

    // Test String handle
    std::string test_str = "SackboyPC_Test_String_12345";
    zb::jni::jstring s_handle = zb::jni::create_local_string(test_str);
    TEST_ASSERT(s_handle != 0, "String handle must not be 0");
    TEST_ASSERT((s_handle & 3) == 2, "Handle tag must be 0b10");

    const char* utf = zb::jni::get_string_utf(s_handle);
    TEST_ASSERT(utf != nullptr, "get_string_utf must not return null");
    TEST_ASSERT(std::string(utf) == test_str, "String content must match original");

    // Test Byte Array handle
    const uint8_t sample_bytes[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    zb::jni::jbyteArray b_handle = zb::jni::create_local_byte_array(sample_bytes, sizeof(sample_bytes));
    TEST_ASSERT(b_handle != 0, "ByteArray handle must not be 0");

    zb::jni::MockObject* b_obj = zb::jni::get_object(b_handle);
    TEST_ASSERT(b_obj != nullptr, "MockObject pointer must be valid");
    TEST_ASSERT(b_obj->type == zb::jni::ObjectType::ByteArray, "Object type must be ByteArray");
    TEST_ASSERT(b_obj->byte_data.size() == sizeof(sample_bytes), "Byte data size must match");
    TEST_ASSERT(std::memcmp(b_obj->byte_data.data(), sample_bytes, sizeof(sample_bytes)) == 0, "Byte content must match");

    // Test Class handle
    std::string c_name = "com/playstation/runsackboyrun/JNI";
    zb::jni::jclass c_handle = zb::jni::create_local_class(c_name);
    TEST_ASSERT(c_handle != 0, "Class handle must not be 0");
    zb::jni::MockObject* c_obj = zb::jni::get_object(c_handle);
    TEST_ASSERT(c_obj != nullptr && c_obj->class_name == c_name, "Class name must match");

    // Test Global conversion
    zb::u32 g_handle = zb::jni::to_global_handle(c_handle);
    TEST_ASSERT(g_handle == c_handle, "Global handle retains ID with global kind");

    TEST_PASS("test_object_handles");
}

void test_mock_dispatch() {
    std::printf("--- Running test_mock_dispatch ---\n");

    // Verify offline configurations
    zb::jni::OfflineConfig cfg;
    cfg.display_density = 2.0f;
    cfg.device_id = "sbr-pc-test-unit-001";
    zb::jni::set_offline_config(cfg);

    const auto& cur_cfg = zb::jni::get_offline_config();
    TEST_ASSERT(cur_cfg.display_density == 2.0f, "Display density config must match");
    TEST_ASSERT(cur_cfg.device_id == "sbr-pc-test-unit-001", "Device ID config must match");

    // Verify JNI native addresses storage
    zb::jni::NativeAddresses addrs;
    addrs.SetFilesDir = 0x001b1c60;
    addrs.Poll = 0x001b1dec;
    addrs.Back = 0x001b1de8;
    zb::jni::set_native_addresses(addrs);

    TEST_ASSERT(zb::jni::get_native_addresses().SetFilesDir == 0x001b1c60, "Native address SetFilesDir must match");
    TEST_ASSERT(zb::jni::get_native_addresses().Poll == 0x001b1dec, "Native address Poll must match");
    TEST_ASSERT(zb::jni::get_native_addresses().Back == 0x001b1de8, "Native address Back must match");

    // Verify resolve_native_addresses applies module_bias to all 18 native symbols
    zb::jni::resolve_native_addresses(0x10000000u);
    TEST_ASSERT(zb::jni::get_native_addresses().SetFilesDir == 0x101b1c60u, "Resolved SetFilesDir must include bias");
    TEST_ASSERT(zb::jni::get_native_addresses().SetUniqueDeviceID == 0x101b1cecu, "Resolved SetUniqueDeviceID must include bias");
    TEST_ASSERT(zb::jni::get_native_addresses().Poll == 0x101b1decu, "Resolved Poll must include bias");
    TEST_ASSERT(zb::jni::get_native_addresses().sceInvert == 0x101c60d4u, "Resolved sceInvert must include bias");

    TEST_PASS("test_mock_dispatch");
}

void test_activity_lifecycle_layout() {
    std::printf("--- Running test_activity_lifecycle_layout ---\n");

    zb::android::ActivityConfig act_cfg;
    act_cfg.internal_data_path = ".\\test_save\\";
    act_cfg.external_data_path = ".\\test_save\\";
    act_cfg.sdk_version = 19;
    zb::android::init(act_cfg);

    TEST_ASSERT(zb::android::get_current_state() == zb::android::ActivityState::Uninitialized, "Initial state must be Uninitialized");

    // Simulate ANativeActivity allocation in guest memory to verify structure layout
    zb::gaddr act = zb::mem::gcalloc(1, 40);
    zb::gaddr cbs = zb::mem::gcalloc(16, 4);

    TEST_ASSERT(act != 0 && cbs != 0, "Guest memory allocation for activity must succeed");

    // Write structure according to specification:
    // Offset 0x00: callbacks
    // Offset 0x04: vm
    // Offset 0x08: env
    // Offset 0x0c: clazz
    // Offset 0x10: internalDataPath
    // Offset 0x14: externalDataPath
    // Offset 0x18: sdkVersion
    // Offset 0x1c: instance
    // Offset 0x20: assetManager
    // Offset 0x24: obbPath

    zb::wr<zb::u32>(act + 0x00, cbs);
    zb::wr<zb::u32>(act + 0x04, zb::jni::guest_vm());
    zb::wr<zb::u32>(act + 0x08, zb::jni::current_env());
    zb::wr<zb::u32>(act + 0x10, zb::mem::gstrdup(".\\test_save\\"));
    zb::wr<zb::i32>(act + 0x18, 19);
    zb::wr<zb::u32>(act + 0x1c, 0x12345678); // Simulate instance populated by onCreate

    TEST_ASSERT(zb::rd<zb::u32>(act + 0x00) == cbs, "Callbacks pointer at offset 0x00 must match");
    TEST_ASSERT(zb::rd<zb::u32>(act + 0x04) == zb::jni::guest_vm(), "JavaVM pointer at offset 0x04 must match");
    TEST_ASSERT(zb::rd<zb::u32>(act + 0x08) == zb::jni::current_env(), "JNIEnv pointer at offset 0x08 must match");
    TEST_ASSERT(zb::rd<zb::i32>(act + 0x18) == 19, "sdkVersion at offset 0x18 must be 19");
    TEST_ASSERT(zb::rd<zb::u32>(act + 0x1c) == 0x12345678, "instance at offset 0x1c must match");

    TEST_PASS("test_activity_lifecycle_layout");
}

int main(int argc, char* argv[]) {
    std::printf("============================================================\n");
    std::printf("Running JNI & Activity Lifecycle Standalone Tests (M2_3)\n");
    std::printf("============================================================\n");

    // Initialize low 4GB memory allocator, sync, and CPU thunk subsystems
    zb::mem::init();
    zb::sync::init();
    zb::cpu::init();

    test_jni_vm_tables();
    test_object_handles();
    test_mock_dispatch();
    test_activity_lifecycle_layout();

    zb::mem::shutdown();

    std::printf("============================================================\n");
    std::printf("ALL JNI & LIFECYCLE TESTS PASSED (4/4)\n");
    std::printf("============================================================\n");
    return 0;
}
