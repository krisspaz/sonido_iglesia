"""Musical, bounded reference matching for offline recommendations."""
from __future__ import annotations

from typing import Any

from .advanced import analyze_full


def _limited(value: float) -> float:
    return round(max(-3.0, min(3.0, value)), 2)


def reference_match(source: dict[str, Any], reference_path: str) -> dict[str, Any]:
    """Compare coarse smoothed bands; never generate a destructive EQ copy."""
    reference = analyze_full(reference_path)
    source_bands, reference_bands = source.get("tonal_balance", {}), reference.get("tonal_balance", {})
    # Ratios are normalized power estimates. Scale deliberately stays modest and
    # is clamped to a musical maximum of +/- 3 dB per band.
    target = {name: _limited((float(reference_bands.get(name, 0.0)) - float(source_bands.get(name, 0.0))) * 12.0)
              for name in ("subgrave", "grave", "low_mid_mud", "mid", "presence", "high", "air")}
    stereo = None
    if source.get("stereo_correlation") is not None and reference.get("stereo_correlation") is not None:
        stereo = round(float(source["stereo_correlation"]) - float(reference["stereo_correlation"]), 3)
    return {"source": source.get("file"), "reference": reference.get("file"),
            "loudness_delta_lufs": round(float(source["lufs_integrated"]) - float(reference["lufs_integrated"]), 2),
            "dynamic_delta_db": round(float(source["crest_factor_db"]) - float(reference["crest_factor_db"]), 2),
            "stereo_correlation_delta": stereo,
            "source_tonal_balance": source_bands, "reference_tonal_balance": reference_bands,
            "target_curve_db": target, "max_band_adjustment_db": 3.0,
            "note": "Curva suavizada y limitada a ±3 dB por banda; es una referencia musical, no una copia de EQ."}
