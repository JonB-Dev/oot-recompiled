import unittest
from tools.summarize_macos_probe import summarize

class ProbeSummaryTest(unittest.TestCase):
    def test_columns_are_resolved_by_name_and_window_is_inclusive(self):
        lines = ['# present_max_us line presents present_mean_us',
                 '70000 1 19 52000', '55000 2 20 50000', '60000 3 21 48000']
        result = summarize(lines, 2, 3)
        self.assertEqual(result['mean_presents_per_second'], 20.5)
        self.assertEqual(result['worst_present_ms'], 60)
        self.assertEqual(result['mean_present_ms'], 49)
        self.assertEqual(result['minimum_presents_per_second'], 20)
    def test_incomplete_or_duplicate_samples_are_rejected(self):
        for rows in (['1 20 50000 55000'], ['1 20 50000 55000'] * 2):
            with self.assertRaises(ValueError):
                summarize(['# line presents present_mean_us present_max_us'] + rows, 1, 2)
    def test_nonfinite_samples_are_rejected(self):
        with self.assertRaises(ValueError):
            summarize(['# line presents present_mean_us present_max_us', '1 nan 50000 55000'], 1, 1)
