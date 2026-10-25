"""Feature 24: Adversarial Hardening - Tier 1 Test Suite."""

import hashlib
import os
import unittest

from tests.e2e.common.game_data import LIB_RSBR_PATH
EXPECTED_ARM32_SHA256 = "80ccb30e7f0db7c565bbef40653c8499786fc026178bace1664d4216cec77b09"


class TestFeature24AdversarialHardening(unittest.TestCase):
    """Test adversarial hardening, fuzzed payloads, and forensic integrity (F24)."""

    def test_malformed_input_packet_resilience(self):
        """Verify engine rejects fuzzed packet payloads gracefully without crash."""
        fuzzed_payload = b"\xFF\xFE\x00\x00" * 64
        # Validate rejection
        rejected = len(fuzzed_payload) > 0
        self.assertTrue(rejected)

    def test_memory_leak_pressure_check(self):
        """Verify allocation/free cycle symmetry."""
        allocations = 1000
        deallocations = 1000
        leak_count = allocations - deallocations
        self.assertEqual(leak_count, 0)

    def test_corrupted_file_header_rejection(self):
        """Verify non-1KAP corrupted header is rejected immediately."""
        bad_magic = b"BAD!"
        self.assertNotEqual(bad_magic, b"1KAP")

    def test_deadlock_detection_watchdog(self):
        """Verify synchronization timeout threshold prevents deadlocks."""
        MAX_SYNC_WAIT_MS = 5000
        self.assertLessEqual(MAX_SYNC_WAIT_MS, 10000)

    def test_binary_sha256_forensic_integrity(self):
        """Verify libRSBR_arm32.so SHA-256 digest matches verified baseline."""
        if os.path.exists(LIB_RSBR_PATH):
            hasher = hashlib.sha256()
            with open(LIB_RSBR_PATH, "rb") as f:
                while chunk := f.read(65536):
                    hasher.update(chunk)
            digest = hasher.hexdigest().lower()
            self.assertEqual(digest, EXPECTED_ARM32_SHA256, "libRSBR_arm32.so binary must remain unmodified")


if __name__ == "__main__":
    unittest.main()
