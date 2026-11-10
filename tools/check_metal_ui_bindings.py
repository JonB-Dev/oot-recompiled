"""Reject texture/sampler aliasing in generated Metal UI shaders."""

import sys
from pathlib import Path


def validate(source: str) -> None:
    if "Overlapping binding:" in source or "reinterpret_cast<constant sampler" in source:
        raise ValueError("Metal UI texture and sampler argument IDs overlap")


if __name__ == "__main__":
    try:
        validate(Path(sys.argv[1]).read_text())
    except ValueError as error:
        sys.exit(str(error))
