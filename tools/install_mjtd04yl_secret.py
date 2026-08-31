#!/usr/bin/env python3
"""Install the one-time retrieved MJTD04YL LTMK without printing it."""

from __future__ import annotations

import json
import os
from pathlib import Path


def main() -> None:
    source = Path("/tmp/mjtd04yl-ltmk.json")
    destination = Path(__file__).resolve().parents[1] / "esphome" / "secrets.yaml"
    value = json.loads(source.read_text(encoding="utf-8"))["gatt_ltmk"]
    if len(value) != 64 or any(ch not in "0123456789abcdefABCDEF" for ch in value):
        raise SystemExit("retrieved LTMK has an invalid format")

    key = "mjtd04yl_gatt_ltmk:"
    lines = destination.read_text(encoding="utf-8").splitlines() if destination.exists() else []
    replacement = f'{key} "{value.lower()}"'
    updated = []
    replaced = False
    for line in lines:
        if line.lstrip().startswith(key):
            updated.append(replacement)
            replaced = True
        else:
            updated.append(line)
    if not replaced:
        updated.append(replacement)

    destination.write_text("\n".join(updated) + "\n", encoding="utf-8")
    os.chmod(destination, 0o600)
    print("MJTD04YL secret installed (value not displayed)")


if __name__ == "__main__":
    main()
