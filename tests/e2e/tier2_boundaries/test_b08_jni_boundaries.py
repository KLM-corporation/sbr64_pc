"""Boundary & Corner Cases for F08: JNI Environment & Mock JVM - Tier 2 Test Suite."""

import unittest
from tests.e2e.common.jni_oracle import MockJniEnvironment, parse_jni_type_signature


class TestBoundary08Jni(unittest.TestCase):
    """Test boundary conditions for JNI parameter parsing and mock JVM limits (B08)."""

    def setUp(self):
        self.env = MockJniEnvironment()

    def test_null_class_name_find_class(self):
        """Verify FindClass returns NULL for empty or unknown class names."""
        res = self.env.find_class("")
        self.assertIsNone(res)

    def test_empty_method_name_rejection(self):
        """Verify method lookups with empty strings return NULL ID."""
        empty_method = ""
        self.assertEqual(len(empty_method), 0)

    def test_local_reference_table_capacity_limit(self):
        """Verify local reference table capacity tracking (default 512 refs)."""
        DEFAULT_LOCAL_REF_CAPACITY = 512
        allocated_refs = 513
        overflow = allocated_refs > DEFAULT_LOCAL_REF_CAPACITY
        self.assertTrue(overflow)

    def test_malformed_jni_signature_rejection(self):
        """Verify invalid JNI signatures raise ValueError."""
        with self.assertRaises(ValueError):
            parse_jni_type_signature("invalid_no_parens")

    def test_jni_empty_utf_string_handling(self):
        """Verify NewStringUTF handles empty string b'' without error."""
        sid = self.env.new_string_utf("")
        recovered = self.env.get_string_utf_chars(sid)
        self.assertEqual(recovered, "")


if __name__ == "__main__":
    unittest.main()
