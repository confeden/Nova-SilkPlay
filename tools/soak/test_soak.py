import ctypes
import unittest

import soak


class SoakValidationTest(unittest.TestCase):
    def test_teardown_dips_cannot_hide_engaged_growth(self):
        samples = [(60, 500), (70, 510), (80, 520), (90, 100), (100, 100), (110, 550)]
        log = "[0] engaged, mode=mc\n[85000] pipeline disengaged\n[105000] engaged, mode=mc\n"
        self.assertLess(soak.compute_growth_per_hour(samples)[3], 0)
        filtered = soak.settled_samples(samples, log)
        self.assertAlmostEqual(soak.compute_growth_per_hour(filtered)[3], 3600)

    def test_settling_and_legacy_scout_events(self):
        log = "[1000] engaged, mode=mc\n[5000] windowed video: looking for the video\n"
        self.assertEqual(soak.settled_samples([(2, 50), (4, 60), (6, 10)], log), [(4, 60)])
        self.assertEqual(soak.settled_samples([(10, 50)], ""), [])

    def test_waiting_is_not_generation(self):
        self.assertEqual(soak.engine_activity("waiting for Chrome\n"),
                         {"overlay_shown": 0, "active_stats": 0})
        activity = soak.engine_activity("overlay shown\nin 24 fps | out 0.0 fps | mc | SHOWN\n")
        self.assertEqual(activity["active_stats"], 0)
        activity = soak.engine_activity("overlay shown\nin 24 fps | out 60.0 fps | mc | hidden\n")
        self.assertEqual(activity["active_stats"], 0)

    def test_active_output(self):
        activity = soak.engine_activity("overlay shown\nin 24 fps | out 59.8 fps | mc | SHOWN\n")
        self.assertEqual(activity, {"overlay_shown": 1, "active_stats": 1})

    def test_warmup_uses_elapsed_time(self):
        samples = [(10, 500), (60, 10), (70, 11), (80, 12)]
        self.assertAlmostEqual(soak.compute_growth_per_hour(samples)[3], 360)

    def test_handles_are_pointer_sized(self):
        self.assertEqual(ctypes.sizeof(soak.kernel32.OpenProcess.restype),
                         ctypes.sizeof(ctypes.c_void_p))
        self.assertEqual(ctypes.sizeof(soak.kernel32.CreateToolhelp32Snapshot.restype),
                         ctypes.sizeof(ctypes.c_void_p))


if __name__ == "__main__":
    unittest.main()
