from __future__ import annotations

import argparse
from .analyzer import analyze_file, write_json


def main() -> None:
    parser = argparse.ArgumentParser(description="Analiza una grabación de culto y propone ajustes seguros")
    parser.add_argument("audio", help="WAV PCM mono/estéreo")
    parser.add_argument("-o", "--output", default="church-report.json", help="Archivo JSON de salida")
    args = parser.parse_args()
    report = analyze_file(args.audio)
    write_json(report, args.output)
    print(f"Reporte creado: {args.output}")
    for item in report.recommendations:
        print(f"[{item.severity}] {item.message} {item.action}")


if __name__ == "__main__":
    main()
