import copy
import unittest

from windowed_check import compare, validate_result


class WindowedVerdictTest(unittest.TestCase):
    def setUp(self):
        self.result = dict.fromkeys((
            "found_s", "scroll_hide_s", "scroll_reshown_s", "theater_hide_s",
            "theater_reshown_s", "pause_hide_s", "play_reshown_s",
            "fullscreen_shown_s", "unfullscreen_shown_s", "clipped_hide_s", "clipped_return_s"), 0.1)
        for key in ("default", "scrolled", "theater", "unfullscreen"):
            self.result[key] = dict.fromkeys(("dl", "dt", "dr", "db"), 0)
        self.result.update(fullscreen_rect=[3200, 0, 1920, 1080],
                           fullscreen_monitor=[3200, 0, 1920, 1080], errors=[], clipped_stayed_hidden=True)

    def test_complete_run(self):
        self.assertEqual(validate_result(self.result), [])

    def test_missing_transition(self):
        for key in tuple(self.result):
            if key.endswith("_s"):
                with self.subTest(key=key):
                    result = copy.deepcopy(self.result)
                    result[key] = None
                    self.assertTrue(validate_result(result))

    def test_wrong_geometry_or_monitor(self):
        self.result["scrolled"]["dr"] = 3
        self.assertTrue(validate_result(self.result))
        self.result["scrolled"]["dr"] = 2
        self.assertEqual(validate_result(self.result), [])
        self.result["fullscreen_rect"][0] = 0
        self.assertTrue(validate_result(self.result))

    def test_empty_and_error_run(self):
        self.assertTrue(validate_result({}))
        self.result["errors"] = ["FAILED"]
        self.assertTrue(validate_result(self.result))

    def test_focus_change_is_a_failure(self):
        self.result["foreground_unchanged"] = False
        self.assertTrue(validate_result(self.result))

    def test_clipped_video_must_stay_hidden(self):
        for value in (None, False):
            with self.subTest(value=value):
                self.result["clipped_stayed_hidden"] = value
                self.assertTrue(validate_result(self.result))

    def test_scroll_compares_only_visible_video(self):
        result = compare([20, 100, 200, 150],
                         {"screen": [20, 50, 200, 200], "viewport": [0, 100, 400, 300]})
        self.assertEqual([result[k] for k in ("dl", "dt", "dr", "db")], [0, 0, 0, 0])
        self.assertIsNone(compare([0, 0, 100, 100],
                                 {"screen": [0, -200, 100, 100], "viewport": [0, 0, 400, 300]}))


if __name__ == "__main__":
    unittest.main()
