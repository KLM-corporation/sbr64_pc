"""JNI environment, JVM function pointer tables, and DEX method signature oracle."""

import re
from typing import Dict, List, Optional, Tuple

# The 18 JNI native methods present in libRSBR_arm32.so and called from Java
JNI_NATIVE_SYMBOLS = [
    "Java_com_playstation_runsackboyrun_JNI_SetFilesDir",
    "Java_com_playstation_runsackboyrun_JNI_SetUniqueDeviceID",
    "Java_com_playstation_runsackboyrun_JNI_Poll",
    "Java_com_playstation_runsackboyrun_JNI_Back",
    "Java_com_playstation_runsackboyrun_JNI_JNIEnterBackgroundState",
    "Java_com_playstation_runsackboyrun_JNI_JNIEnterForegroundState",
    "Java_com_playstation_runsackboyrun_JNI_JNIAudioSetGain",
    "Java_com_playstation_runsackboyrun_JNI_JNIRenderInit",
    "Java_com_playstation_runsackboyrun_JNI_JNIRenderResize",
    "Java_com_playstation_runsackboyrun_JNI_JNIRenderDraw",
    "Java_com_playstation_runsackboyrun_JNI_TouchDown",
    "Java_com_playstation_runsackboyrun_JNI_TouchUp",
    "Java_com_playstation_runsackboyrun_JNI_TouchMove",
    "Java_com_playstation_runsackboyrun_JNI_NotifyPurchased",
    "Java_com_playstation_runsackboyrun_JNI_NotifyConsumed",
    "Java_com_playstation_runsackboyrun_JNI_NotifyQueryPurchases",
    "Java_com_playstation_runsackboyrun_JNI_NotifyProductDetails",
    "Java_com_playstation_runsackboyrun_JNI_NotifyPurchaseFailed",
]


def parse_jni_type_signature(sig: str) -> Tuple[List[str], str]:
    """Parse JNI method signature (e.g., '(Ljava/lang/String;I)Z') into parameter types and return type."""
    if not sig.startswith("("):
        raise ValueError(f"Invalid JNI signature: {sig}")
    end_paren = sig.find(")")
    if end_paren == -1:
        raise ValueError(f"Missing closing parenthesis in JNI signature: {sig}")

    param_part = sig[1:end_paren]
    return_part = sig[end_paren + 1 :]

    type_map = {
        "V": "void",
        "Z": "boolean",
        "B": "byte",
        "C": "char",
        "S": "short",
        "I": "int",
        "J": "long",
        "F": "float",
        "D": "double",
    }

    def tokenize(s: str) -> List[str]:
        tokens = []
        i = 0
        while i < len(s):
            c = s[i]
            if c in type_map:
                tokens.append(type_map[c])
                i += 1
            elif c == "[":
                # Array type
                sub_tokens = tokenize(s[i + 1 :])
                if sub_tokens:
                    tokens.append(f"{sub_tokens[0]}[]")
                    # Advance past array token
                    if s[i + 1] == "L":
                        semi = s.find(";", i + 1)
                        i = semi + 1
                    else:
                        i += 2
                else:
                    i += 1
            elif c == "L":
                semi = s.find(";", i)
                if semi == -1:
                    tokens.append(s[i:])
                    break
                cls_name = s[i + 1 : semi].replace("/", ".")
                tokens.append(cls_name)
                i = semi + 1
            else:
                i += 1
        return tokens

    params = tokenize(param_part)
    ret_tokens = tokenize(return_part)
    ret = ret_tokens[0] if ret_tokens else "void"
    return params, ret


class MockJniEnvironment:
    """Mock JNIEnv and JavaVM state tracker for testing HLE interactions."""

    def __init__(self):
        self.classes: Dict[str, Dict] = {
            "android/app/NativeActivity": {},
            "android/os/Environment": {},
            "java/io/File": {},
            "java/lang/ClassLoader": {},
            "com/playstation/runsackboyrun/JNI": {},
        }
        self.strings: Dict[int, str] = {}
        self.next_string_id = 0x1000
        self.static_method_calls: List[Tuple[str, str, Tuple]] = []

    def find_class(self, class_name: str) -> Optional[int]:
        normalized = class_name.replace(".", "/")
        if normalized in self.classes:
            return hash(normalized) & 0x7FFFFFFF
        return None

    def new_string_utf(self, text: str) -> int:
        sid = self.next_string_id
        self.strings[sid] = text
        self.next_string_id += 4
        return sid

    def get_string_utf_chars(self, sid: int) -> Optional[str]:
        return self.strings.get(sid)

    def call_static_void_method(self, cls_name: str, method_name: str, *args):
        self.static_method_calls.append((cls_name, method_name, args))
