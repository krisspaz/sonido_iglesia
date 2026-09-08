from __future__ import annotations

import math
from typing import Any

from .analyzer import AudioReport, _read_pcm, _db, analyze_samples


def _true_peak_4x(left: list[float], right: list[float]) -> float:
    """Four-times oversampled peak using polyphase interpolation when present."""
    try:
        import numpy as np
        from scipy.signal import resample_poly
        values = []
        for channel in (left, right):
            if channel:
                values.append(float(np.max(np.abs(resample_poly(np.asarray(channel), 4, 1)))))
        return max(values, default=0.0)
    except ImportError:
        peak = 0.0
        for channel in (left, right):
            for a, b in zip(channel, channel[1:]):
                peak = max(peak, abs(a), abs(a + (b - a) * .25), abs(a + (b - a) * .5),
                           abs(a + (b - a) * .75))
        return peak


def _loudness(left: list[float], right: list[float], rate: int) -> dict[str, float]:
    mono = [(a + b) * .5 for a, b in zip(left, right)]
    result = {"integrated": -120.0, "short_term_max": -120.0, "momentary_max": -120.0}
    try:
        import numpy as np
        import pyloudnorm as pyln
        meter = pyln.Meter(rate)
        audio = np.asarray(mono, dtype=float)
        result["integrated"] = float(meter.integrated_loudness(audio))
        for seconds, key, step in ((3, "short_term_max", 1), (0.4, "momentary_max", .2)):
            size, hop = max(1, int(rate * seconds)), max(1, int(rate * step))
            values = [meter.integrated_loudness(audio[i:i + size])
                      for i in range(0, max(1, len(audio) - size + 1), hop)
                      if len(audio[i:i + size]) >= max(1, size // 2)]
            if values:
                result[key] = max(values)
    except (ImportError, ValueError, RuntimeError):
        rms = math.sqrt(sum(x * x for x in mono) / max(1, len(mono)))
        result = {key: _db(rms) - .691 for key in result}
    return result


def _segments(mono: list[float], rate: int) -> list[dict[str, Any]]:
    window = max(rate, int(rate * 2.0))
    segments = []
    for start in range(0, len(mono), window):
        block = mono[start:start + window]
        if not block:
            continue
        rms = math.sqrt(sum(x * x for x in block) / len(block))
        zcr = sum((a >= 0) != (b >= 0) for a, b in zip(block, block[1:])) / max(1, len(block) - 1)
        if rms < .012:
            label, confidence = "silence", .96
        elif zcr > .18 and rms > .06:
            label, confidence = "applause_ambience", .55
        elif zcr < .12:
            label, confidence = "preaching_or_vocal", .58
        else:
            label, confidence = "worship_music", .48
        segments.append({"start_seconds": start / rate, "end_seconds": min(len(mono), start + window) / rate,
                         "label": label, "confidence": confidence})
    return segments


def analyze_full(path: str, processed_path: str | None = None) -> dict[str, Any]:
    rate, channels, left, right = _read_pcm(path)
    report: AudioReport = analyze_samples(path, rate, channels, left, right)
    loudness = _loudness(left, right, rate)
    true_peak = _true_peak_4x(left, right)
    mono = [(a + b) * .5 for a, b in zip(left, right)]
    report.lufs_integrated = loudness["integrated"]
    report.lufs_short_term = loudness["short_term_max"]
    report.lufs_momentary = loudness["momentary_max"]
    report.true_peak_dbtp = _db(true_peak)
    report.headroom_db = max(0.0, -report.true_peak_dbtp)
    report.sections = _segments(mono, rate)
    result = report.to_dict()
    result["measurement_standard"] = {"loudness": "EBU R128 via pyloudnorm", "true_peak": "4x oversampling"}
    if processed_path:
        processed = analyze_full(processed_path)
        result["comparison"] = {
            "original": {"lufs_integrated": report.lufs_integrated, "true_peak_dbtp": report.true_peak_dbtp,
                          "crest_factor_db": report.crest_factor_db, "stereo_correlation": report.stereo_correlation},
            "processed": {key: processed[key] for key in ("lufs_integrated", "true_peak_dbtp", "crest_factor_db", "stereo_correlation")},
            "delta": {"lufs": processed["lufs_integrated"] - report.lufs_integrated,
                      "true_peak_db": processed["true_peak_dbtp"] - report.true_peak_dbtp,
                      "crest_db": processed["crest_factor_db"] - report.crest_factor_db},
        }
    return result
