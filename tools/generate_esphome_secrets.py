#!/usr/bin/env python3
"""Create missing ESPHome credentials without displaying their values."""

from __future__ import annotations

import argparse
import base64
import os
import secrets
from pathlib import Path


DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / "esphome" / "secrets.yaml"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    path = args.output
    lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
    keys = {line.split(":", 1)[0].strip() for line in lines if ":" in line}
    generated = {
        "esphome_api_encryption_key": base64.b64encode(os.urandom(32)).decode("ascii"),
        "esphome_ota_password": secrets.token_urlsafe(24),
        "fallback_ap_password": secrets.token_urlsafe(12),
    }
    for key, value in generated.items():
        if key not in keys:
            lines.append(f'{key}: "{value}"')

    placeholders = {
        "mjtd04yl_mac_address": "AA:BB:CC:DD:EE:FF",
        "mjtd04yl_gatt_ltmk": "REPLACE_WITH_64_HEXADECIMAL_CHARACTERS",
        "indoor_temperature_entity_id": "sensor.indoor_temperature",
        "indoor_humidity_entity_id": "sensor.indoor_humidity",
        "outdoor_weather_entity_id": "weather.home",
        "pm25_entity_id": "sensor.indoor_pm25",
        "voc_entity_id": "sensor.indoor_voc",
    }
    for key, value in placeholders.items():
        if key not in keys:
            lines.append(f'{key}: "{value}"')

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    os.chmod(path, 0o600)
    print(f"ESPHome secrets prepared at {path} (values not displayed)")


if __name__ == "__main__":
    main()
