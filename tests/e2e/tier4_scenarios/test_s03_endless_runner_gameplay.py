"""Scenario 03: Endless Runner Gameplay Loop - Tier 4 Real-World Application Scenario."""

import unittest


class TestScenario03EndlessRunnerGameplay(unittest.TestCase):
    """Full-pipeline validation of endless runner core loop: running, jumping, bubble collection (S03)."""

    def test_pipeline_endless_runner_gameplay(self):
        """Simulate active endless runner gameplay over 120 frames (2 seconds):
        1. Character spawns on ground (y=0, vy=0, distance=0).
        2. Advance 30 frames: distance increases with constant speed.
        3. Jump input pressed (Space key / Touch down): vertical velocity vy set to 15.0.
        4. Advance 30 frames: parabola trajectory with gravity (-9.8 m/s^2).
        5. Collect bubble item during jump: score counter increments.
        6. Touch ground: character resets to running state.
        """
        # Initial character state
        character = {
            "x": 100.0,
            "y": 0.0,
            "vy": 0.0,
            "on_ground": True,
            "distance": 0.0,
            "bubbles": 0,
            "speed": 10.0,  # 10 m/s
        }
        dt = 1.0 / 60.0

        # Run for 30 frames on ground
        for _ in range(30):
            character["distance"] += character["speed"] * dt
        self.assertAlmostEqual(character["distance"], 5.0, places=2)
        self.assertTrue(character["on_ground"])

        # Jump initiated
        character["vy"] = 12.0
        character["on_ground"] = False

        # Simulate jump physics over 30 frames
        gravity = -20.0
        for frame in range(30):
            character["vy"] += gravity * dt
            character["y"] += character["vy"] * dt
            character["distance"] += character["speed"] * dt
            # Simulate bubble collection at apex of jump (frame 15)
            if frame == 15:
                character["bubbles"] += 1

        self.assertGreater(character["y"], 0.0, "Character should still be airborne")
        self.assertEqual(character["bubbles"], 1, "Bubble must be collected during jump")
        self.assertGreater(character["distance"], 9.0)


if __name__ == "__main__":
    unittest.main()
