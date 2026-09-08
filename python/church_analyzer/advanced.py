"""Offline measurements. This module never opens or controls live audio."""
from __future__ import annotations

import math
from typing import Any

from .analyzer import AudioReport, _band_ratio, _db, _read_pcm, analyze_samples

_BANDS = {"subgrave": (20., 60.), "grave": (60., 180.), "low_mid_mud": (200., 400.),
          "mid": (400., 2000.), "presence": (2500., 5000.), "high": (5000., 10000.), "air": (10000., 18000.)}


def _true_peak_4x(left: list[float], right: list[float]) -> tuple[float, bool]:
    try:
        import numpy as np
        from scipy.signal import resample_poly
        return max((float(np.max(np.abs(resample_poly(np.asarray(c), 4, 1)))) for c in (left, right) if c), default=0.), True
    except ImportError:
        peak = max((abs(x) for c in (left, right) for x in c), default=0.)
        for c in (left, right):
            for a, b in zip(c, c[1:]):
                peak = max(peak, abs(a + (b - a) * .25), abs(a + (b - a) * .5), abs(a + (b - a) * .75))
        return peak, False


def _loudness(left: list[float], right: list[float], rate: int) -> tuple[dict[str, Any], bool]:
    mono = [(a + b) * .5 for a, b in zip(left, right)]
    output: dict[str, Any] = {"integrated": -120., "short_term_max": -120., "momentary_max": -120., "short_term": [], "momentary": []}
    try:
        import numpy as np
        import pyloudnorm as pyln
        meter, audio = pyln.Meter(rate), np.asarray(mono, dtype=float)
        output["integrated"] = float(meter.integrated_loudness(audio))
        for seconds, name, step in ((3., "short_term", 1.), (.4, "momentary", .1)):
            size, hop, points = max(1, int(rate * seconds)), max(1, int(rate * step)), []
            for index in range(0, max(1, len(audio) - size + 1), hop):
                chunk = audio[index:index + size]
                if len(chunk) == size:
                    try:
                        points.append({"time_seconds": round((index + size) / rate, 3), "lufs": round(float(meter.integrated_loudness(chunk)), 2)})
                    except ValueError:
                        pass
            output[name] = points
            output[f"{name}_max"] = max((p["lufs"] for p in points), default=-120.)
        return output, True
    except (ImportError, ValueError, RuntimeError):
        estimate = _db(math.sqrt(sum(x * x for x in mono) / max(1, len(mono)))) - .691
        output.update({"integrated": estimate, "short_term_max": estimate, "momentary_max": estimate})
        return output, False


def _tonal(mono: list[float], rate: int) -> dict[str, float]:
    nyquist = rate / 2. - 1.
    return {name: round(_band_ratio(mono, rate, lo, min(hi, nyquist)), 5) if lo < nyquist else 0.
            for name, (lo, hi) in _BANDS.items()}


def _segments(mono: list[float], rate: int) -> list[dict[str, Any]]:
    window, raw = max(rate, int(rate * 2)), []
    for start in range(0, len(mono), window):
        block = mono[start:start + window]
        if not block:
            continue
        rms = math.sqrt(sum(x*x for x in block) / len(block))
        zcr = sum((a >= 0) != (b >= 0) for a, b in zip(block, block[1:])) / max(1, len(block)-1)
        high = _band_ratio(block, rate, 1800, min(7000, rate / 2 - 1))
        if rms < .012: label, confidence = "silence", .96
        elif zcr > .18 and rms > .06 and high > .08: label, confidence = "applause_ambience", .55
        elif zcr < .12 and high < .12: label, confidence = "preaching", .58
        else: label, confidence = "worship", .48
        current = {"start_seconds": round(start/rate, 3), "end_seconds": round(min(len(mono), start+window)/rate, 3),
                   "label": label, "confidence": confidence, "method": "acoustic_heuristic"}
        if raw and raw[-1]["label"] == label:
            raw[-1]["end_seconds"] = current["end_seconds"]
        else:
            raw.append(current)
    return raw


def _moments(mono: list[float], rate: int, tonal: dict[str, float]) -> list[dict[str, Any]]:
    output = []
    for start in range(0, len(mono), max(1, rate)):
        block = mono[start:start + rate]
        if max((abs(x) for x in block), default=0.) >= .999:
            output.append({"start_seconds": round(start/rate, 3), "end_seconds": round((start+len(block))/rate, 3),
                           "kind": "clipping", "severity": "high", "reason": "Pico digital cercano a 0 dBFS"})
    if tonal["low_mid_mud"] > .10:
        output.append({"start_seconds": 0., "end_seconds": round(len(mono)/rate, 3), "kind": "mud", "severity": "medium", "reason": "Energía persistente en 200–400 Hz"})
    return output[:100]


def _add(recs: list[dict], kind: str, message: str, action: str, confidence: float, **values: Any) -> None:
    if not any(item.get("kind") == kind for item in recs):
        recs.append({"kind": kind, "module": kind, "severity": "medium", "message": message, "action": action,
                     "confidence": confidence, **values})


def _recommend(report: dict[str, Any], tonal: dict[str, float], intelligibility: dict[str, Any]) -> list[dict[str, Any]]:
    recs = list(report["recommendations"])
    if tonal["low_mid_mud"] > .10:
        _add(recs, "dynamic_eq_mud", "El low-mid puede tapar la voz.", "Reducción dinámica conservadora en 200–400 Hz.", .78, frequency_hz=280, gain_db=-1.5)
    if tonal["presence"] > .12 or tonal["high"] > .16:
        _add(recs, "dynamic_eq_harshness", "Presencia/agudos elevados pueden volver agresivos los platillos.", "Atenuación dinámica moderada en 2.5–5 kHz.", .74, frequency_hz=3500, gain_db=-1.25)
        _add(recs, "de_esser", "Contenido sibilante o platillos elevado.", "De-esser suave 5–8 kHz, máximo 3 dB de reducción.", .61, frequency_hz=6500, gain_db=-2.)
    if report["crest_factor_db"] < 6.:
        _add(recs, "compressor", "La mezcla tiene poco margen dinámico.", "Evite comprimir más; pruebe ratio 1.5:1, ataque 20 ms, release 150 ms.", .70, ratio=1.5, attack_ms=20, release_ms=150)
    elif report["crest_factor_db"] > 18.:
        _add(recs, "compressor", "Hay picos considerables.", "Pruebe ratio 2:1, ataque 25 ms, release 180 ms antes del limitador.", .68, ratio=2., attack_ms=25, release_ms=180)
    if report["true_peak_dbtp"] > -1.:
        _add(recs, "limiter", "El true peak deja poco margen de codificación.", "Ajuste el techo final a -1 dBTP.", .91, ceiling_dbtp=-1.)
    if intelligibility["score"] < .55:
        _add(recs, "voice_clarity", "La claridad vocal aproximada es baja.", "Reduzca mud, reverb o música antes de subir compresión.", intelligibility["confidence"])
    return recs


def analyze_full(path: str, processed_path: str | None = None) -> dict[str, Any]:
    rate, channels, left, right = _read_pcm(path)
    base: AudioReport = analyze_samples(path, rate, channels, left, right)
    mono, loudness = [(a+b)*.5 for a, b in zip(left, right)], None
    loudness, r128 = _loudness(left, right, rate)
    peak, exact_peak = _true_peak_4x(left, right)
    tonal = _tonal(mono, rate)
    intelligibility = {"score": round(max(0., min(1., .52 + (tonal["mid"] + tonal["presence"])*.8 - (tonal["low_mid_mud"] + tonal["high"])*.7)), 2),
                        "confidence": .42, "method": "spectral_proxy", "note": "Estimación; mejora con stems Demucs y Whisper."}
    base.lufs_integrated, base.lufs_short_term, base.lufs_momentary = loudness["integrated"], loudness["short_term_max"], loudness["momentary_max"]
    base.true_peak_dbtp, base.headroom_db, base.sections = _db(peak), max(0., -_db(peak)), _segments(mono, rate)
    result = base.to_dict()
    result.update({"tonal_balance": tonal, "intelligibility": intelligibility, "problem_moments": _moments(mono, rate, tonal),
                   "loudness_timeline": {"short_term": loudness["short_term"], "momentary": loudness["momentary"]},
                   "measurement_standard": {"loudness": "EBU R128" if r128 else "RMS approximation (install analysis dependencies for EBU R128)",
                                            "true_peak": "4x polyphase oversampling" if exact_peak else "4x linear approximation (install scipy for polyphase)"}})
    result["recommendations"] = _recommend(result, tonal, intelligibility)
    if processed_path:
        processed = analyze_full(processed_path)
        keys = ("lufs_integrated", "true_peak_dbtp", "crest_factor_db", "stereo_correlation", "tonal_balance", "intelligibility")
        result["comparison"] = {"original": {k: result[k] for k in keys}, "processed": {k: processed[k] for k in keys},
                                "delta": {"lufs": round(processed["lufs_integrated"]-result["lufs_integrated"], 2), "true_peak_db": round(processed["true_peak_dbtp"]-result["true_peak_dbtp"], 2), "crest_db": round(processed["crest_factor_db"]-result["crest_factor_db"], 2), "intelligibility": round(processed["intelligibility"]["score"]-intelligibility["score"], 2)}}
    return result


def refine_with_transcript(report: dict[str, Any], transcript: dict[str, Any]) -> dict[str, Any]:
    """Refine acoustic sections from local Whisper timestamps when available.

    Whisper detects speech, not worship genre; uncovered acoustic regions retain
    their conservative heuristic labels.  This makes the provenance visible to
    the operator instead of pretending an ASR model has perfect segmentation.
    """
    speech = [segment for segment in transcript.get("segments", [])
              if isinstance(segment, dict) and isinstance(segment.get("start"), (int, float))
              and isinstance(segment.get("end"), (int, float)) and segment["end"] > segment["start"]]
    if not speech:
        return report
    sections = list(report.get("sections", []))
    for section in sections:
        start, end = section["start_seconds"], section["end_seconds"]
        spoken = sum(max(0., min(end, float(item["end"])) - max(start, float(item["start"]))) for item in speech)
        if spoken / max(.001, end - start) > .35:
            section.update({"label": "preaching", "confidence": .78, "method": "whisper_timestamp"})
    report["sections"] = sections
    report["optional_modules"]["transcription"] = "Whisper timestamps applied locally"
    return report
