#!/usr/bin/env python3
"""TallyPad listener: receives button presses from the pad over the LAN
and turns them into one plain-text event line.

The pad firmware POSTs JSON to /press:  {"button": 1..8, "battery": null}
Button meanings live in buttons.json and can change without a reflash —
the file is re-read on every event.

What happens with the event line is up to you, via config.json:
  {"webhook_url": "https://your-endpoint", "webhook_headers": {"X-Token": "..."}}
Leave webhook_url empty (or omit config.json) and events are just logged,
which is enough to wire in Home Assistant, MQTT, a spreadsheet — anything.

Presses within COMBINE_WINDOW_S of the first are merged into ONE event
("pee + poo + cloth"), because one diaper change is one event no matter
how many buttons describe it. The event is stamped with the FIRST press.

Two optional extras:
  - A button whose meaning in buttons.json is "IGNORE" is silently dropped
    (handy for a test button). An event made only of ignored buttons sends
    nothing.
  - Buttons listed in config.json "immediate_buttons" (e.g. [3, 7]) skip
    the combine window and are sent the moment they are pressed, with that
    minute. Good for start/stop toggles where exact order and time matter.
"""
import json
import logging
import threading
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = 4180
COMBINE_WINDOW_S = 10.0
IGNORE = "IGNORE"

log = logging.getLogger("tallypad-listener")


def load_buttons():
    return json.loads((HERE / "buttons.json").read_text())


def load_config():
    path = HERE / "config.json"
    if not path.exists():
        return {}
    return json.loads(path.read_text())


def immediate_buttons():
    return set(load_config().get("immediate_buttons") or [])


def filter_ignored(buttons_pressed, buttons):
    return [b for b in buttons_pressed if buttons.get(str(b)) != IGNORE]


def format_presses(buttons_pressed, buttons, now=None):
    """One plain-text line for one combined event. Multiple buttons within
    the window are one event. Unmapped buttons still make a record."""
    now = now or datetime.now().astimezone()
    hhmm = now.strftime("%H:%M")
    parts, unmapped = [], []
    for b in buttons_pressed:
        label = buttons.get(str(b))
        (parts if label else unmapped).append(label or f"button {b}")
    desc = " + ".join(parts + unmapped)
    extra = " (unmapped button)" if unmapped else ""
    return f"pad press {hhmm}: {desc}{extra}"


class Combiner:
    """Collects presses for COMBINE_WINDOW_S after the first, then posts once."""

    def __init__(self, post_fn):
        self.post_fn = post_fn
        self.lock = threading.Lock()
        self.pending = []
        self.timer = None
        self.first_at = None

    def add(self, button):
        with self.lock:
            if button not in self.pending:
                self.pending.append(button)
            if self.timer is None:
                self.first_at = datetime.now().astimezone()
                self.timer = threading.Timer(COMBINE_WINDOW_S, self.flush)
                self.timer.daemon = True
                self.timer.start()

    def flush(self):
        with self.lock:
            pressed, self.pending, self.timer = self.pending, [], None
            first_at, self.first_at = self.first_at, None
        if not pressed:
            return
        buttons = load_buttons()
        kept = filter_ignored(pressed, buttons)
        if not kept:
            log.info("ignored-only press %s (nothing sent)", pressed)
            return
        try:
            self.post_fn(format_presses(kept, buttons, now=first_at))
            log.info("combined event posted: buttons %s", kept)
        except Exception as e:
            log.error("combined post failed for %s: %s", kept, e)


def handle_press(button, deliver, combiner, now=None):
    """Route one press: 'posted' (immediate), 'ignored', or 'queued'."""
    if button in immediate_buttons():
        buttons = load_buttons()
        if not filter_ignored([button], buttons):
            return "ignored"
        now = now or datetime.now().astimezone()
        deliver(format_presses([button], buttons, now=now))
        log.info("immediate event posted: button %s", button)
        return "posted"
    combiner.add(button)
    return "queued"


def deliver(text):
    config = load_config()
    url = config.get("webhook_url")
    log.info("EVENT: %s", text)
    if not url:
        return
    headers = {"Content-Type": "application/json"}
    headers.update(config.get("webhook_headers") or {})
    req = urllib.request.Request(
        url,
        data=json.dumps({"text": text}).encode(),
        headers=headers,
        method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/health":
            self._reply(200, "ok")
        else:
            self._reply(404, "not found")

    def do_POST(self):
        if self.path != "/press":
            self._reply(404, "not found")
            return
        try:
            body = json.loads(self.rfile.read(
                int(self.headers.get("Content-Length", 0))))
            button = int(body["button"])
        except (ValueError, KeyError, json.JSONDecodeError):
            self._reply(400, "bad request")
            return
        try:
            result = handle_press(button, deliver, COMBINER)
        except Exception as e:
            log.error("immediate post failed for %s: %s", button, e)
            self._reply(502, "post failed")
            return
        log.info("press button=%s %s battery=%s",
                 button, result, body.get("battery"))
        self._reply(200, result)

    def _reply(self, code, text):
        body = text.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        log.info("%s %s", self.address_string(), fmt % args)


COMBINER = Combiner(deliver)


def main():
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    load_buttons()
    HTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
