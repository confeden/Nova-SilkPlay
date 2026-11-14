import tempfile
import unittest
from pathlib import Path

from yt_check import summarize_dda


class DdaSpanTests(unittest.TestCase):
    def summarize(self, accumulated):
        with tempfile.TemporaryDirectory(prefix='nsp-dda-summary-') as folder:
            prefix = Path(folder) / 'capture'
            rows = ['# capture geometry', 'index,qpc_ms,last_present_qpc_ms,accumulated,hash']
            rows.extend(f'{i},{1000+i*10},{1000+i*10},{count},{i:x}'
                        for i, count in enumerate(accumulated))
            prefix.with_suffix('.csv').write_text('\n'.join(rows), encoding='utf8')
            return summarize_dda(str(prefix))

    def test_initial_backlog_precedes_measured_span(self):
        result = self.summarize([4, 1, 1])
        self.assertEqual(result['recorder_missed'], 0)
        self.assertEqual(result['recorder_startup_missed'], 3)
        self.assertEqual(result['span_s'], .02)
        self.assertEqual(result['distinct_pictures_per_s'], 100)

    def test_later_accumulation_is_a_recording_loss(self):
        result = self.summarize([1, 4, 1])
        self.assertEqual(result['recorder_missed'], 3)
        self.assertEqual(result['recorder_startup_missed'], 0)

    def test_both_counts_remain_visible(self):
        result = self.summarize([3, 2, 1])
        self.assertEqual(result['recorder_missed'], 1)
        self.assertEqual(result['recorder_startup_missed'], 2)


if __name__ == '__main__':
    unittest.main()
