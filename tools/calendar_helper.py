#!/usr/bin/env python3
"""MeetingNotifier — local Google Calendar helper.

Speaks the same JSON contract the firmware's calendar_json.h expects, so
you can point CALENDAR_URL at this helper (running on your Mac / Pi /
whatever) instead of an Apps Script web app. Useful when your work
Workspace has Apps Script or external sharing disabled.

Setup (one time):

    1. https://console.cloud.google.com → New project (free).
    2. APIs & Services → Library → enable "Google Calendar API".
    3. APIs & Services → Credentials → Create credentials →
       OAuth client ID → "Desktop app". Download the JSON to
       `tools/client_secret.json`.
    4. pip install -r tools/requirements.txt
    5. python tools/calendar_helper.py auth
       (a browser tab opens, sign in with the calendar account, allow read access)

Daily use:

    python tools/calendar_helper.py serve

Then in your firmware build:

    pio run -e esp32-c3-devkitm-1 \\
      -t upload \\
      -DCALENDAR_URL='"http://<your-host>.local:8080/"'

The host must be reachable from the ESP32 on your LAN. `0.0.0.0` is the
default bind. If mDNS is flaky on your network, use the LAN IP.
"""

from __future__ import annotations

import argparse
import datetime as dt
import http.server
import json
import socketserver
import sys
from pathlib import Path

try:
    from google.auth.transport.requests import Request
    from google.oauth2.credentials import Credentials
    from google_auth_oauthlib.flow import InstalledAppFlow
    from googleapiclient.discovery import build
except ImportError as exc:
    sys.exit(
        f"missing dependency: {exc.name}\n"
        "install with: pip install -r tools/requirements.txt"
    )

SCOPES = ["https://www.googleapis.com/auth/calendar.readonly"]
HERE = Path(__file__).resolve().parent
CLIENT_SECRET = HERE / "client_secret.json"
TOKEN_PATH = HERE / ".token.json"


def load_creds() -> Credentials | None:
    if not TOKEN_PATH.exists():
        return None
    creds = Credentials.from_authorized_user_file(str(TOKEN_PATH), SCOPES)
    if creds.expired and creds.refresh_token:
        creds.refresh(Request())
        TOKEN_PATH.write_text(creds.to_json())
    return creds if creds.valid else None


def cmd_auth() -> None:
    if not CLIENT_SECRET.exists():
        sys.exit(f"missing {CLIENT_SECRET} — see the docstring at top of file")
    flow = InstalledAppFlow.from_client_secrets_file(str(CLIENT_SECRET), SCOPES)
    creds = flow.run_local_server(port=0)
    TOKEN_PATH.write_text(creds.to_json())
    print(f"token written to {TOKEN_PATH}")


def fetch_next() -> dict:
    creds = load_creds()
    if not creds:
        raise RuntimeError("not authenticated — run: calendar_helper.py auth")

    service = build("calendar", "v3", credentials=creds, cache_discovery=False)
    now = dt.datetime.now(dt.timezone.utc)
    eod_local = dt.datetime.now().astimezone().replace(
        hour=23, minute=59, second=59, microsecond=0
    )

    items = (
        service.events()
        .list(
            calendarId="primary",
            timeMin=now.isoformat(),
            timeMax=eod_local.astimezone(dt.timezone.utc).isoformat(),
            singleEvents=True,
            orderBy="startTime",
            maxResults=50,
        )
        .execute()
        .get("items", [])
    )

    timed = [e for e in items if "dateTime" in e.get("start", {})]
    if not timed:
        return {"status": "clear"}

    def start_of(e):
        return dt.datetime.fromisoformat(e["start"]["dateTime"])

    def end_of(e):
        return dt.datetime.fromisoformat(e["end"]["dateTime"])

    current = next((e for e in timed if start_of(e) <= now <= end_of(e)), None)
    upcoming = [e for e in timed if start_of(e) > now]
    next_evt = upcoming[0] if upcoming else None
    focus = current or next_evt
    if focus is None:
        return {"status": "clear"}

    return {
        "status": "in_meeting" if current else "upcoming",
        "title": focus.get("summary", ""),
        "start": focus["start"]["dateTime"],
        "end": focus["end"]["dateTime"],
        "location": focus.get("location") or focus.get("hangoutLink") or "",
        "next_title": next_evt["summary"] if current and next_evt else "",
        "next_start": next_evt["start"]["dateTime"] if current and next_evt else "",
        "remaining_today": len(upcoming),
    }


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self) -> None:  # noqa: N802
        try:
            payload = fetch_next()
            body = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        except Exception as exc:
            msg = f"{type(exc).__name__}: {exc}".encode()
            self.send_response(500)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(msg)))
            self.end_headers()
            self.wfile.write(msg)

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write(f"[{self.log_date_time_string()}] {fmt % args}\n")


def cmd_serve(host: str, port: int) -> None:
    if not load_creds():
        sys.exit("not authenticated — run: calendar_helper.py auth")
    with socketserver.TCPServer((host, port), Handler) as srv:
        print(f"serving on http://{host}:{port}/  (Ctrl-C to stop)")
        srv.serve_forever()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("auth", help="run one-time OAuth flow")
    s = sub.add_parser("serve", help="run HTTP server on the LAN")
    s.add_argument("--host", default="0.0.0.0")
    s.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()

    if args.cmd == "auth":
        cmd_auth()
    else:
        cmd_serve(args.host, args.port)


if __name__ == "__main__":
    main()
