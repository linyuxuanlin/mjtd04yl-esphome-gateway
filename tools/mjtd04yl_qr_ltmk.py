#!/usr/bin/env python3
"""One-shot Xiaomi QR login and MJTD04YL GATT LTMK retrieval.

Account tokens are kept in memory only. The resulting device key is written to
the explicit output path with owner-only permissions and is never printed.
"""

import argparse
import asyncio
import base64
import hashlib
import json
import os
import time
from pathlib import Path
from urllib.parse import quote, urlencode

import aiohttp

from miservice import MiAccount, MiIOService


ACCOUNT_UA = (
    "APP/com.xiaomi.mihome APPV/6.0.103 "
    "iosPassportSDK/3.9.0 iOS/14.4 miHSTS"
)
SID = "xiaomiio"


def parse_xiaomi_json(text: str) -> dict:
    if text.startswith("&&&START&&&"):
        text = text[11:]
    return json.loads(text)


async def retrieve(did: str, qr_path: Path, output_path: Path) -> None:
    device_id = hashlib.md5(
        f"mjtd04yl-qr-{time.time_ns()}".encode()
    ).hexdigest()[:16]

    timeout = aiohttp.ClientTimeout(total=25)
    async with aiohttp.ClientSession(timeout=timeout) as session:
        qr_params = urlencode(
            {
                "_qrsize": "480",
                "qs": "%3Fsid%3Dxiaomiio%26_json%3Dtrue",
                "callback": "https://sts.api.io.mi.com/sts",
                "_hasLogo": "false",
                "sid": SID,
                "serviceParam": "",
                "_locale": "zh_CN",
                "_dc": str(int(time.time() * 1000)),
            },
            quote_via=quote,
        )
        async with session.get(
            f"https://account.xiaomi.com/longPolling/loginUrl?{qr_params}",
            headers={"User-Agent": ACCOUNT_UA},
            cookies={"sdkVersion": "accountsdk-18.8.15", "deviceId": device_id},
        ) as response:
            login_info = parse_xiaomi_json(await response.text())

        qr_url = login_info.get("qr")
        polling_url = login_info.get("lp")
        expires = int(login_info.get("timeout", 300))
        if not qr_url or not polling_url:
            raise RuntimeError("Xiaomi did not return a QR login URL")

        async with session.get(qr_url) as response:
            qr_bytes = await response.read()
        qr_path.write_bytes(qr_bytes)
        qr_path.chmod(0o600)
        print(f"QR_READY {qr_path}", flush=True)

        login_result = None
        deadline = time.monotonic() + expires
        while time.monotonic() < deadline:
            try:
                async with session.get(
                    polling_url, headers={"User-Agent": ACCOUNT_UA}
                ) as response:
                    if response.status != 200:
                        continue
                    candidate = parse_xiaomi_json(await response.text())
                if candidate.get("passToken"):
                    login_result = candidate
                    break
                polling_url = candidate.get("lp") or polling_url
            except (asyncio.TimeoutError, aiohttp.ClientError, json.JSONDecodeError):
                continue

        if not login_result:
            raise RuntimeError("QR login expired before confirmation")
        print("QR_CONFIRMED", flush=True)

        nonce = login_result["nonce"]
        ssecurity = login_result["ssecurity"]
        client_sign = base64.b64encode(
            hashlib.sha1(f"nonce={nonce}&{ssecurity}".encode()).digest()
        ).decode()
        location = f"{login_result['location']}&clientSign={quote(client_sign)}"
        async with session.get(
            location, headers={"User-Agent": "APP/com.xiaomi.mihome"}
        ) as response:
            service_cookie = response.cookies.get("serviceToken")
            if not service_cookie:
                raise RuntimeError("Xiaomi did not issue a serviceToken")
            service_token = service_cookie.value

        account = MiAccount(session, "", "", token_store=None)
        account.token = {
            "deviceId": device_id,
            "userId": str(login_result["userId"]),
            "passToken": login_result["passToken"],
            SID: (ssecurity, service_token),
        }
        service = MiIOService(account=account, region="cn")
        result = await service.miio_request(
            "/v2/blemesh/query_dev", {"did": str(did)}
        )
        ltmk = result.get("gatt_ltmk") if isinstance(result, dict) else None
        if not isinstance(ltmk, str) or not ltmk:
            keys = sorted(result.keys()) if isinstance(result, dict) else []
            raise RuntimeError(f"gatt_ltmk missing; result keys: {keys}")
        if any(ch not in "0123456789abcdefABCDEF" for ch in ltmk):
            raise RuntimeError("gatt_ltmk has an unexpected encoding")

        output_path.write_text(json.dumps({"gatt_ltmk": ltmk}))
        output_path.chmod(0o600)
        print(f"LTMK_READY length={len(ltmk)} output={output_path}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--did", required=True)
    parser.add_argument("--qr", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    asyncio.run(retrieve(args.did, args.qr, args.output))


if __name__ == "__main__":
    main()
