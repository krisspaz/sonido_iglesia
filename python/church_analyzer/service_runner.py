import argparse
import os

import uvicorn

from church_analyzer.api import create_app


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Church Sound Analyst local service")
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--database")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    os.environ["CHURCH_ANALYZER_TOKEN_FILE"] = args.token_file
    if args.database:
        os.environ["CHURCH_ANALYZER_DB"] = args.database
    uvicorn.run(create_app(), host="127.0.0.1", port=args.port)
