from __future__ import annotations

from .analyzer import AudioReport, analyze_file


def reference_match(source: AudioReport, reference_path: str) -> dict:
    reference = analyze_file(reference_path)
    def limited(value: float) -> float:
        return round(max(-3.0, min(3.0, value)), 2)
    return {
        "source": source.file,
        "reference": reference.file,
        "loudness_delta_lufs": round(source.lufs_integrated - reference.lufs_integrated, 2),
        "dynamic_delta_db": round(source.crest_factor_db - reference.crest_factor_db, 2),
        "stereo_delta": None if source.stereo_correlation is None or reference.stereo_correlation is None else round(source.stereo_correlation - reference.stereo_correlation, 3),
        "target_curve_db": {
            "subgrave": limited((reference.low_energy_ratio - source.low_energy_ratio) * 8),
            "mud_200_400": limited((reference.low_energy_ratio - source.low_energy_ratio) * 4),
            "presence_2_5k": 0.0,
            "air": limited((reference.high_energy_ratio - source.high_energy_ratio) * 8),
        },
        "note": "Curva limitada a ±3 dB; es una referencia musical, no una copia de EQ.",
    }
