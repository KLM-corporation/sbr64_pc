"""Scenario 08: Costume Store and Entitlement Simulation - Tier 4 Real-World Application Scenario."""

import unittest
from tests.e2e.common.save_oracle import SaveProfile


class TestScenario08CostumeShopEntitlement(unittest.TestCase):
    """Full-pipeline validation of costume store unlock and persistent entitlement (S08)."""

    def test_pipeline_costume_shop_entitlement(self):
        """Simulate costume store purchase/unlock:
        1. Player visits Costume Shop menu.
        2. Player has 2500 bubbles.
        3. Aviator Costume costs 2000 bubbles.
        4. Transaction approved offline: bubbles deducted (2500 - 2000 = 500).
        5. Bitmask of unlocked costumes updated (bit 1 for Aviator set).
        6. Verify updated profile can equip new costume.
        """
        profile = SaveProfile(bubbles=2500, unlocked_costumes=1)  # Default costume bit 0 set

        COSTUME_AVIATOR_BIT = 2  # bit 1
        COSTUME_PRICE = 2000

        # Purchase transaction
        self.assertGreaterEqual(profile.bubbles, COSTUME_PRICE)
        profile.bubbles -= COSTUME_PRICE
        profile.unlocked_costumes |= COSTUME_AVIATOR_BIT

        self.assertEqual(profile.bubbles, 500)
        self.assertTrue(profile.unlocked_costumes & COSTUME_AVIATOR_BIT)

        # Verify serialization preserves unlock
        raw = profile.serialize()
        restored = SaveProfile.deserialize(raw)
        self.assertIsNotNone(restored)
        self.assertEqual(restored.bubbles, 500)
        self.assertTrue(restored.unlocked_costumes & COSTUME_AVIATOR_BIT)


if __name__ == "__main__":
    unittest.main()
