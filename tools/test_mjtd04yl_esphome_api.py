#!/usr/bin/env python3
"""Exercise the local MJTD04YL ESPHome light without exposing API secrets."""

from __future__ import annotations

import argparse
import asyncio
from pathlib import Path

from aioesphomeapi import APIClient, LightInfo, LightState


def read_flat_secret(secrets_path: Path, key: str) -> str:
    for line in secrets_path.read_text(encoding="utf-8").splitlines():
        if line.split(":", 1)[0].strip() == key and ":" in line:
            return line.split(":", 1)[1].split("#", 1)[0].strip().strip('"\'')
    raise RuntimeError(f"Secret {key!r} not found in {secrets_path}")


def read_noise_key(config_path: Path) -> str:
    secrets_path = config_path.parent / "secrets.yaml"
    if secrets_path.exists():
        try:
            return read_flat_secret(secrets_path, "esphome_api_encryption_key")
        except RuntimeError:
            pass

    # Backward compatibility with the pre-1.5 single-file configuration.
    in_api = False
    in_encryption = False
    for line in config_path.read_text(encoding="utf-8").splitlines():
        if line and not line[0].isspace():
            in_api = line.strip() == "api:"
            in_encryption = False
            continue
        stripped = line.strip()
        if in_api and stripped == "encryption:":
            in_encryption = True
        elif in_api and in_encryption and stripped.startswith("key:"):
            value = stripped.removeprefix("key:").split("#", 1)[0].strip()
            if value.startswith("!secret "):
                return read_flat_secret(config_path.parent / "secrets.yaml", value.removeprefix("!secret ").strip())
            return value.strip('"\'')
    raise RuntimeError(f"ESPHome API encryption key not found in {config_path}")


async def run_test(host: str, config_path: Path) -> None:
    client = APIClient(host, 6053, None, noise_psk=read_noise_key(config_path))
    states: dict[int, LightState] = {}
    state_event = asyncio.Event()

    def on_state(state: object) -> None:
        if isinstance(state, LightState):
            states[state.key] = state
            print(
                "STATE"
                f" key={state.key} on={state.state}"
                f" brightness={state.brightness:.3f}"
                f" color_temperature={state.color_temperature:.1f}",
                flush=True,
            )
            state_event.set()

    await client.connect(login=True)
    try:
        device_info, entities, _ = await client.device_info_and_list_entities()
        print(f"DEVICE name={device_info.name} model={device_info.model}")
        lights = [entity for entity in entities if isinstance(entity, LightInfo)]
        for light in lights:
            print(f"LIGHT name={light.name!r} object_id={light.object_id!r} key={light.key}")
        light = next((item for item in lights if "台灯" in item.name), None)
        if light is None:
            raise RuntimeError("MJTD04YL light entity was not advertised by the ESPHome node")

        client.subscribe_states(on_state)
        try:
            await asyncio.wait_for(state_event.wait(), timeout=5)
        except TimeoutError:
            print("WARN no initial light state callback received", flush=True)

        print("COMMAND turn_on brightness=0.35 color_temperature=250.0mired", flush=True)
        state_event.clear()
        client.light_command(light.key, state=True, brightness=0.35, color_temperature=250.0)
        await asyncio.sleep(4)

        print("COMMAND turn_off", flush=True)
        state_event.clear()
        client.light_command(light.key, state=False)
        await asyncio.sleep(4)
    finally:
        await client.disconnect()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True, help="ESPHome node IP address or hostname")
    parser.add_argument(
        "--config",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "esphome" / "beetle-esp32-c3.yaml",
    )
    args = parser.parse_args()
    asyncio.run(run_test(args.host, args.config))


if __name__ == "__main__":
    main()
