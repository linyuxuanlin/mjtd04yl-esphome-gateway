#!/usr/bin/env python3
"""Probe only the public first step of MJTD04YL Mesh login from macOS BLE."""

from __future__ import annotations

import asyncio

from bleak import BleakClient, BleakScanner


AUTH_CONTROL = "00000010-0000-1000-8000-00805f9b34fb"
AUTH_DATA = "00000016-0000-1000-8000-00805f9b34fb"


async def main() -> None:
    print("Scanning for MJTD04YL...")
    device = await BleakScanner.find_device_by_filter(
        lambda dev, adv: adv.local_name == "yeelink.light.lamp21",
        timeout=20.0,
    )
    if device is None:
        raise SystemExit("MJTD04YL not found")
    print(f"Found {device.name}; connecting")

    events: asyncio.Queue[tuple[str, bytes]] = asyncio.Queue()

    def auth_control(_, data: bytearray) -> None:
        events.put_nowait(("auth-control", bytes(data)))

    def auth_data(_, data: bytearray) -> None:
        events.put_nowait(("auth-data", bytes(data)))

    async with BleakClient(device, timeout=20.0) as client:
        await client.start_notify(AUTH_CONTROL, auth_control)
        await client.start_notify(AUTH_DATA, auth_data)
        await asyncio.sleep(0.3)
        print("Writing login start byte A4")
        await client.write_gatt_char(AUTH_CONTROL, b"\xA4", response=False)
        try:
            channel, payload = await asyncio.wait_for(events.get(), timeout=8.0)
        except TimeoutError:
            print("NO_NOTIFICATION")
            return
        print(f"NOTIFICATION {channel} len={len(payload)} value={payload.hex()}")


if __name__ == "__main__":
    asyncio.run(main())
