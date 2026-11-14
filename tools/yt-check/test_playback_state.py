import unittest

from yt_arms import media_time_reached, playable_state


class PlaybackStateTest(unittest.TestCase):
    def test_replacing_video_is_not_ready(self):
        for state in (None, {}, {"w": None, "t": None, "paused": None},
                      {"w": 0, "t": 0, "paused": False},
                      {"w": 1920, "t": 10, "paused": True}):
            self.assertFalse(playable_state(state, 1920))

    def test_rendition_gate(self):
        self.assertFalse(playable_state({"w": 1280, "t": 10, "paused": False}, 1920))
        self.assertTrue(playable_state({"w": 1920, "t": 10, "paused": False}, 1920))

    def test_bad_clocks_never_start_a_recording(self):
        for value in (None, "240", float("nan"), float("inf"), True):
            self.assertFalse(media_time_reached(value, 240))
        self.assertFalse(media_time_reached(239.99, 240))
        self.assertTrue(media_time_reached(240.0, 240))


if __name__ == "__main__":
    unittest.main()
