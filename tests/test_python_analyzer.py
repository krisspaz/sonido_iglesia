import math
import struct
import tempfile
import unittest
import wave
from pathlib import Path

from church_analyzer.analyzer import analyze_file


class PythonAnalyzerTests(unittest.TestCase):
    def make_wav(self, samples, channels=2, rate=8000):
        handle = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
        handle.close()
        path = Path(handle.name)
        with wave.open(str(path), "wb") as output:
            output.setnchannels(channels)
            output.setsampwidth(2)
            output.setframerate(rate)
            if channels == 1:
                output.writeframes(b"".join(struct.pack("<h", x) for x in samples))
            else:
                output.writeframes(b"".join(struct.pack("<hh", x, x) for x in samples))
        self.addCleanup(path.unlink, missing_ok=True)
        return path

    def test_metrics_and_json_shape(self):
        samples = [int(0.2 * 32767 * math.sin(2 * math.pi * 440 * i / 8000)) for i in range(8000)]
        report = analyze_file(self.make_wav(samples))
        self.assertEqual(report.sample_rate, 8000)
        self.assertEqual(report.channels, 2)
        self.assertAlmostEqual(report.stereo_correlation, 1.0, places=3)
        self.assertEqual(report.clipping_ratio, 0.0)
        self.assertIn("recommendations", report.to_dict())

    def test_silence_is_detected(self):
        report = analyze_file(self.make_wav([0] * 8000, channels=1))
        self.assertEqual(report.silence_ratio, 1.0)
        self.assertTrue(any(item.kind == "routing" for item in report.recommendations))


if __name__ == "__main__":
    unittest.main()
