import argparse
import os
from pathlib import Path

import uvicorn

from church_analyzer.api import create_app


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Church Sound Analyst local service")
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--database")
    parser.add_argument("--data-dir")
    parser.add_argument("--allowed-root", action="append", default=[])
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    if not Path(args.token_file).is_file():
        parser.error("--token-file debe existir y ser creado por la aplicación")
    if not 1024 <= args.port <= 65535:
        parser.error("--port debe estar entre 1024 y 65535")
    os.environ["CHURCH_ANALYZER_TOKEN_FILE"] = args.token_file
    if args.database:
        os.environ["CHURCH_ANALYZER_DB"] = args.database
    if args.data_dir:
        os.environ["CHURCH_ANALYZER_DATA_DIR"] = args.data_dir
    if args.allowed_root:
        os.environ["CHURCH_ANALYZER_ALLOWED_ROOTS"] = os.pathsep.join(args.allowed_root)
    uvicorn.run(create_app(), host="127.0.0.1", port=args.port)
