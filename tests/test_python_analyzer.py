import math
import os
import struct
import tempfile
import unittest
import wave
from pathlib import Path

from church_analyzer.analyzer import analyze_file
from church_analyzer.advanced import analyze_full, refine_with_transcript
from church_analyzer.jobs import LocalJobManager
from church_analyzer.learning import LocalLearning
from church_analyzer.reference import reference_match


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

    def test_advanced_report_compares_processed_file(self):
        source = self.make_wav([int(.2 * 32767 * math.sin(2 * math.pi * 440 * i / 8000)) for i in range(8000)])
        processed = self.make_wav([int(.1 * 32767 * math.sin(2 * math.pi * 440 * i / 8000)) for i in range(8000)])
        report = analyze_full(str(source), str(processed))
        self.assertIn("comparison", report)
        self.assertIn("true_peak_dbtp", report)
        self.assertTrue(report["sections"])

    def test_learning_uses_only_excellent_sessions(self):
        handle = tempfile.NamedTemporaryFile(suffix=".sqlite", delete=False)
        database = Path(handle.name)
        handle.close()
        database.unlink()
        self.addCleanup(database.unlink, missing_ok=True)
        learning = LocalLearning(database)
        learning.add("iglesia", "excellent", {"lufs_integrated": -15}, [{"kind": "dynamic_eq_mud"}])
        learning.add("iglesia", "disliked", {"lufs_integrated": -8}, [{"kind": "limiter"}])
        preference = learning.preference("iglesia")
        self.assertEqual(preference["loudness_target"], -15.0)
        self.assertEqual(preference["accepted_modules"], {"dynamic_eq_mud": 1})

    def test_full_report_has_bounded_reference_curve_and_timeline(self):
        source = self.make_wav([int(.2 * 32767 * math.sin(2 * math.pi * 440 * i / 8000)) for i in range(16000)])
        reference = self.make_wav([int(.7 * 32767 * math.sin(2 * math.pi * 60 * i / 8000)) for i in range(16000)])
        report = analyze_full(str(source))
        match = reference_match(report, str(reference))
        self.assertIn("tonal_balance", report)
        self.assertIn("intelligibility", report)
        self.assertIn("measurement_standard", report)
        self.assertLessEqual(max(abs(value) for value in match["target_curve_db"].values()), 3.0)

    def test_job_manager_reports_completion_and_cancellation(self):
        manager = LocalJobManager(max_workers=1)
        try:
            job = manager.submit("test", lambda update, cancelled: {"ok": not cancelled.is_set()})
            job.future.result(timeout=2)
            self.assertEqual(manager.get(job.id).status, "complete")
            queued = manager.submit("cancel", lambda update, cancelled: {"ok": not cancelled.is_set()})
            manager.cancel(queued.id)
            self.assertIn(manager.get(queued.id).status, {"complete", "cancelled"})
        finally:
            manager.shutdown()

    def test_whisper_timestamps_refine_acoustic_sections(self):
        path = self.make_wav([int(.15 * 32767 * math.sin(2 * math.pi * 440 * i / 8000)) for i in range(16000)])
        refined = refine_with_transcript(analyze_full(str(path)), {"segments": [{"start": 0., "end": 2., "text": "prueba"}]})
        self.assertEqual(refined["sections"][0]["label"], "preaching")
        self.assertEqual(refined["sections"][0]["method"], "whisper_timestamp")


@unittest.skipUnless(__import__("importlib").util.find_spec("fastapi"), "requiere extra server")
class LocalApiTests(unittest.TestCase):
    def setUp(self):
        try:
            from fastapi.testclient import TestClient
        except RuntimeError as exc:
            self.skipTest(f"TestClient no disponible: {exc}")
        from church_analyzer.api import create_app
        self.tempdir = tempfile.TemporaryDirectory()
        os.environ["CHURCH_ANALYZER_TOKEN"] = "test-token"
        os.environ["CHURCH_ANALYZER_DATA_DIR"] = self.tempdir.name
        self.client = TestClient(create_app())

    def tearDown(self):
        self.client.close()
        self.tempdir.cleanup()
        os.environ.pop("CHURCH_ANALYZER_TOKEN", None)
        os.environ.pop("CHURCH_ANALYZER_DATA_DIR", None)

    def test_health_auth_and_invalid_path(self):
        self.assertEqual(self.client.get("/health").status_code, 200)
        self.assertEqual(self.client.post("/analyze-full", json={"path": "/not-a-file.wav"}).status_code, 401)
        response = self.client.post("/analyze-full", headers={"x-api-token": "test-token"}, json={"path": "/not-a-file.wav"})
        self.assertEqual(response.status_code, 400)


if __name__ == "__main__":
    unittest.main()
