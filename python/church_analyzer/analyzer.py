from __future__ import annotations

import json
import math
import struct
import wave
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


@dataclass
class Recommendation:
    kind: str
    severity: str
    message: str
    action: str
    confidence: float = 0.0
    frequency_hz: int | None = None
    gain_db: float | None = None


@dataclass
class ProblemMoment:
    start_seconds: float
    end_seconds: float
    kind: str
    reason: str
    severity: str


@dataclass
class AudioReport:
    file: str
    sample_rate: int
    channels: int
    duration_seconds: float
    peak_dbfs: float
    rms_dbfs: float
    crest_factor_db: float
    clipping_ratio: float
    silence_ratio: float
    low_energy_ratio: float
    high_energy_ratio: float
    stereo_correlation: float | None
    recommendations: list[Recommendation]
    lufs_integrated: float
    lufs_short_term: float
    lufs_momentary: float
    true_peak_dbtp: float
    headroom_db: float
    sections: list[dict[str, Any]]
    problem_moments: list[ProblemMoment]
    comparison: dict[str, Any] | None
    optional_modules: dict[str, str]

    def to_dict(self) -> dict:
        return asdict(self)


def _db(value: float, floor: float = -120.0) -> float:
    return max(floor, 20.0 * math.log10(max(value, 10 ** (floor / 20))))


def _read_pcm(path: str | Path) -> tuple[int, int, list[float], list[float]]:
    if Path(path).suffix.lower() in {".flac", ".aif", ".aiff"}:
        try:
            import soundfile as sf
            data, rate = sf.read(str(path), always_2d=True, dtype="float32")
            left = data[:, 0].tolist()
            right = data[:, min(1, data.shape[1] - 1)].tolist()
            return int(rate), int(data.shape[1]), left, right
        except ImportError as exc:
            raise ValueError("FLAC/AIFF requiere instalar el extra 'analysis'") from exc
    with wave.open(str(path), "rb") as source:
        channels, width, rate, frames = (source.getnchannels(), source.getsampwidth(),
                                         source.getframerate(), source.getnframes())
        if channels not in (1, 2) or width not in (1, 2, 3, 4):
            raise ValueError("Se requiere WAV PCM mono o estéreo de 8/16/24/32 bits")
        raw = source.readframes(frames)
    step = width * channels
    left, right = [], []
    scale = 127.0 if width == 1 else float(1 << (width * 8 - 1))
    for offset in range(0, len(raw) - step + 1, step):
        values = []
        for channel in range(channels):
            chunk = raw[offset + channel * width: offset + (channel + 1) * width]
            if width == 1:
                value = chunk[0] - 128
            elif width == 3:
                value = int.from_bytes(chunk, "little", signed=False)
                if value & 0x800000:
                    value -= 1 << 24
            else:
                value = int.from_bytes(chunk, "little", signed=True)
            values.append(value / scale)
        left.append(values[0])
        right.append(values[-1])
    return rate, channels, left, right


def _band_ratio(samples: list[float], rate: int, low: float, high: float) -> float:
    """Approximate band power using a small, dependency-free DFT on downsampled data."""
    if not samples:
        return 0.0
    data = samples[: min(len(samples), rate * 20)]
    n = min(len(data), 4096)
    data = data[:n]
    total = sum(x * x for x in data) or 1e-12
    # Goertzel-like sampling of representative frequencies.
    points = [low * (high / low) ** (i / 7) for i in range(8)] if low else [high / 16]
    power = 0.0
    for freq in points:
        real = imag = 0.0
        for index, sample in enumerate(data):
            angle = 2 * math.pi * freq * index / rate
            real += sample * math.cos(angle)
            imag -= sample * math.sin(angle)
        power += (real * real + imag * imag) / (n * n)
    return min(1.0, power / total)


def analyze_samples(path: str | Path, rate: int, channels: int,
                    left: list[float], right: list[float]) -> AudioReport:
    mono = [(a + b) * 0.5 for a, b in zip(left, right)]
    peak = max((abs(x) for x in mono), default=0.0)
    rms = math.sqrt(sum(x * x for x in mono) / max(1, len(mono)))
    clipped = sum(abs(x) >= 0.999 for x in mono) / max(1, len(mono))
    silent = sum(abs(x) < 0.01 for x in mono) / max(1, len(mono))
    low = _band_ratio(mono, rate, 20, 180)
    high = _band_ratio(mono, rate, 4500, min(16000, rate / 2 - 1))
    correlation = None
    if channels == 2 and left:
        ml, mr = sum(left) / len(left), sum(right) / len(right)
        vl = sum((x - ml) ** 2 for x in left)
        vr = sum((x - mr) ** 2 for x in right)
        correlation = sum((a - ml) * (b - mr) for a, b in zip(left, right)) / math.sqrt(max(vl * vr, 1e-12))
    # LUFS is calculated with pyloudnorm when available. The fallback is an
    # explicitly approximate RMS estimate and never drives live DSP changes.
    lufs_integrated = _db(rms) - 0.691
    lufs_short = lufs_integrated
    lufs_momentary = lufs_integrated
    try:
        import numpy as np
        import pyloudnorm as pyln
        meter = pyln.Meter(rate)
        array = np.asarray(mono, dtype=float)
        lufs_integrated = float(meter.integrated_loudness(array))
        windows = [array[max(0, i - rate * 3):i] for i in range(rate * 3, len(array) + 1, rate)]
        if windows:
            lufs_short = float(max(meter.integrated_loudness(w) for w in windows))
        windows = [array[max(0, i - rate):i] for i in range(rate, len(array) + 1, max(1, rate // 2))]
        if windows:
            lufs_momentary = float(max(meter.integrated_loudness(w) for w in windows))
    except (ImportError, ValueError, RuntimeError):
        pass
    recs: list[Recommendation] = []
    moments: list[ProblemMoment] = []
    if clipped > 0.0001 or peak > 0.98:
        recs.append(Recommendation("limiter", "high", "Hay picos que pueden distorsionar la mezcla.", "Baja la ganancia de entrada 3–6 dB y deja el limitador a -1 dBTP.", .96, None, -1.0))
    if low > 0.28:
        recs.append(Recommendation("dynamic_eq_mud", "medium", "Hay exceso relativo de energía en graves/low-mid.", "Reduce suavemente 200–400 Hz para recuperar claridad vocal.", .86, 280, -1.5))
    if high > 0.22:
        recs.append(Recommendation("dynamic_eq_harshness", "medium", "La zona alta puede resultar agresiva, especialmente en platillos.", "Usa EQ dinámica moderada entre 2.5–5 kHz.", .78, 3500, -1.5))
    if rms < 0.08:
        recs.append(Recommendation("loudness_target", "medium", "El nivel promedio es bajo.", "Revisa ganancia antes de comprimir; apunta a -16 a -14 LUFS integrados.", .82, None, -14.0))
    if silent > 0.35:
        recs.append(Recommendation("routing", "low", "Hay una proporción alta de silencio o señal muy baja.", "Revisa cables, muteos, ruteo y la entrada de OBS.", .74))
    if correlation is not None and correlation < 0.05:
        recs.append(Recommendation("stereo", "high", "La correlación estéreo es baja; puede haber cancelaciones en mono.", "Comprueba polaridad, paneo y compatibilidad mono antes de emitir.", .94))
    if clipped > 0.0001:
        moments.append(ProblemMoment(0.0, len(mono) / rate, "clipping", "Picos cercanos a 0 dBFS", "high"))
    sections = [{"start_seconds": 0.0, "end_seconds": len(mono) / rate,
                 "label": "silence" if silent > .5 else "service_mix", "confidence": .45}]
    return AudioReport(str(path), rate, channels, len(mono) / rate, _db(peak), _db(rms), _db(peak / max(rms, 1e-12)), clipped, silent, low, high, correlation, recs,
                       lufs_integrated, lufs_short, lufs_momentary, _db(peak), max(0.0, -_db(peak)), sections, moments, None,
                       {"transcription": "optional: whisper", "separation": "optional: demucs", "lufs": "pyloudnorm when installed"})


def analyze_file(path: str | Path) -> AudioReport:
    rate, channels, left, right = _read_pcm(path)
    return analyze_samples(path, rate, channels, left, right)


def write_json(report: AudioReport, output: str | Path) -> None:
    Path(output).write_text(json.dumps(report.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8")
