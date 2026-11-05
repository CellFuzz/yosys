#!/usr/bin/env python3
"""Reproduce the pinned fixtures from an Ibex sv2v Verilog bundle, or verify.

The module bodies are copied exactly; unrelated modules and trailing blank
lines are excluded. Different module contents require an explicit fixture
update rather than silently changing the regression's source version.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path)
    parser.add_argument("--output", type=Path, default=HERE)
    args = parser.parse_args()
    metadata = json.loads((HERE / "source.json").read_text())
    bundle = args.source.read_text() if args.source else None
    args.output.mkdir(parents=True, exist_ok=True)
    for name, expected in metadata["modules"].items():
        output = args.output / (name + ".v")
        if bundle is None:
            content = output.read_bytes()
        else:
            matches = re.findall(r"^module " + name + r"\s*\(.*?^endmodule\s*$", bundle, re.MULTILINE | re.DOTALL)
            if len(matches) != 1:
                raise ValueError(f"expected exactly one {name} module")
            content = (matches[0].rstrip() + "\n").encode()
        if hashlib.sha256(content).hexdigest() != expected:
            raise ValueError(f"{name}: source differs from pinned fixture")
        if bundle is not None:
            output.write_bytes(content)
    print("Pinned Ibex fixture hashes verified.")


if __name__ == "__main__":
    main()
