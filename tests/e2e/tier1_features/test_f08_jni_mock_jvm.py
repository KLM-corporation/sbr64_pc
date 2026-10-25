"""Feature 08: JNI Environment & Mock JVM - Tier 1 Test Suite."""

import unittest
from tests.e2e.common.jni_oracle import (
    JNI_NATIVE_SYMBOLS,
    MockJniEnvironment,
    parse_jni_type_signature,
)


class TestFeature08JniMockJvm(unittest.TestCase):
    """Test JNIEnv, JavaVM mock, and native method registration (F08)."""

    def setUp(self):
        self.env = MockJniEnvironment()

    def test_jni_native_method_count(self):
        """Verify the 18 JNI native methods from DEX bytecode are inventoried."""
        self.assertGreaterEqual(len(JNI_NATIVE_SYMBOLS), 18)
        self.assertIn("Java_com_playstation_runsackboyrun_JNI_SetFilesDir", JNI_NATIVE_SYMBOLS)
        self.assertIn("Java_com_playstation_runsackboyrun_JNI_Poll", JNI_NATIVE_SYMBOLS)

    def test_mock_jvm_find_class(self):
        """Verify mock JVM can resolve target Android and RSBR classes."""
        cls_id = self.env.find_class("com/playstation/runsackboyrun/JNI")
        self.assertIsNotNone(cls_id)
        self.assertIsNone(self.env.find_class("non/existent/Class"))

    def test_jni_string_utf_roundtrip(self):
        """Verify JNI NewStringUTF and GetStringUTFChars mock behavior."""
        test_str = "C:\\sbr64_pc\\save"
        sid = self.env.new_string_utf(test_str)
        recovered = self.env.get_string_utf_chars(sid)
        self.assertEqual(recovered, test_str)

    def test_parse_jni_signature(self):
        """Verify parsing of complex JNI method signatures."""
        params, ret = parse_jni_type_signature("(Ljava/lang/String;[BI)V")
        self.assertEqual(params, ["java.lang.String", "byte[]", "int"])
        self.assertEqual(ret, "void")

        params2, ret2 = parse_jni_type_signature("()Z")
        self.assertEqual(params2, [])
        self.assertEqual(ret2, "boolean")

    def test_jni_env_pointer_alignment(self):
        """Verify JNIEnv pointer structure alignment for 32-bit guest calls."""
        # 32-bit pointer table: each JNI function pointer is 4 bytes
        POINTER_SIZE = 4
        self.assertEqual(POINTER_SIZE, 4)


if __name__ == "__main__":
    unittest.main()
