"""
CyberPiercer Tuner — Web-based real-time parameter tuning tool.

Architecture:
    ESP32 <--(serial)--> tuner.py <--(WebSocket)--> browser (tuner.html)

Usage:
    python tuner.py [--port COM5] [--baud 115200] [--web-port 9000]
    Open http://localhost:9000 in browser.

Requires:
    pip install fastapi uvicorn pyserial websockets
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import re
import threading
import time
from pathlib import Path

import serial
import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("tuner")

# ────────────────────── Serial manager ──────────────────────

class SerialManager:
    """Thread-safe serial port manager with command/response and telemetry."""

    # Shell prompt pattern (Zephyr shell outputs "uart:~$ " after each command)
    PROMPT_RE = re.compile(r"uart:~\$\s*$")
    # ANSI escape code stripper
    ANSI_RE = re.compile(r"\x1b\[[0-9;]*[a-zA-Z]")
    # Parameter response: $P name=value unit
    PARAM_RE = re.compile(r"^\$P\s+(\w+)=(-?\d+)\s*(.*)")
    # OK response: $OK name=value unit
    OK_RE = re.compile(r"^\$OK\s+(\w+)=(-?\d+)\s*(.*)")
    # Applied response
    APPLIED_RE = re.compile(r"^\$APPLIED\b")
    # Telemetry (from LOG_INF "TRK ...")
    TELEM_RE = re.compile(
        r"TRK\s+cx=(-?\d+)\s+cy=(-?\d+)\s+H=(\d+)us([M.])\s+V=(\d+)us([M.])"
    )

    def __init__(self, port: str, baud: int) -> None:
        self.port = port
        self.baud = baud
        self._ser: serial.Serial | None = None
        self._lock = threading.Lock()
        self._telem_callbacks: list = []
        self._reader_thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._response_lines: list[str] = []
        self._response_event = threading.Event()

    def connect(self) -> None:
        self._ser = serial.Serial(self.port, self.baud, timeout=0.1)
        self._stop.clear()
        self._reader_thread = threading.Thread(
            target=self._reader_loop, daemon=True, name="serial-reader"
        )
        self._reader_thread.start()
        log.info("Serial connected: %s @ %d", self.port, self.baud)
        # Clear any pending data
        time.sleep(0.3)
        self._ser.reset_input_buffer()

    def close(self) -> None:
        self._stop.set()
        if self._reader_thread:
            self._reader_thread.join(timeout=2)
        if self._ser:
            self._ser.close()
            self._ser = None

    @property
    def connected(self) -> bool:
        return self._ser is not None and self._ser.is_open

    def reconnect(self, port: str | None = None) -> None:
        """Close current connection and reconnect (optionally to a new port)."""
        self.close()
        if port:
            self.port = port
        self.connect()

    def add_telem_callback(self, cb) -> None:
        self._telem_callbacks.append(cb)

    def send_command(self, cmd: str, timeout: float = 2.0) -> list[str]:
        """Send a shell command and collect response lines until next prompt."""
        with self._lock:
            self._response_lines = []
            self._response_event.clear()

            full_cmd = cmd.strip() + "\r\n"
            self._ser.write(full_cmd.encode("utf-8"))
            self._ser.flush()

            # Wait for prompt (response complete)
            self._response_event.wait(timeout=timeout)
            lines = list(self._response_lines)
            self._response_lines = []
            return lines

    def _reader_loop(self) -> None:
        """Background thread: read serial lines, dispatch telemetry, collect responses."""
        buf = ""
        while not self._stop.is_set():
            try:
                data = self._ser.read(self._ser.in_waiting or 1)
                if not data:
                    continue
                buf += data.decode("utf-8", errors="replace")

                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = self.ANSI_RE.sub("", line).strip()
                    if not line:
                        continue

                    # Check for telemetry
                    m = self.TELEM_RE.search(line)
                    if m:
                        telem = {
                            "type": "telem",
                            "ts": time.time(),
                            "cx": int(m.group(1)),
                            "cy": int(m.group(2)),
                            "h_us": int(m.group(3)),
                            "h_moving": m.group(4) == "M",
                            "v_us": int(m.group(5)),
                            "v_moving": m.group(6) == "M",
                        }
                        for cb in self._telem_callbacks:
                            try:
                                cb(telem)
                            except Exception:
                                pass

                    # Collect response lines
                    self._response_lines.append(line)

                    # Check if prompt appeared → response complete
                    if self.PROMPT_RE.search(line):
                        self._response_event.set()

                # Also check remaining buffer for prompt (no trailing newline)
                clean_buf = self.ANSI_RE.sub("", buf).strip()
                if clean_buf and self.PROMPT_RE.search(clean_buf):
                    self._response_event.set()

            except serial.SerialException:
                log.error("Serial disconnected")
                break
            except Exception as e:
                log.error("Serial reader error: %s", e)

    def get_all_params(self) -> dict[str, dict]:
        """Send 'tune get' and parse all $P responses."""
        lines = self.send_command("tune get")
        params = {}
        for line in lines:
            m = self.PARAM_RE.match(line)
            if m:
                params[m.group(1)] = {
                    "value": int(m.group(2)),
                    "unit": m.group(3).strip(),
                }
        return params

    def set_param(self, name: str, value: int) -> dict:
        """Send 'tune set <name> <value>' and return result."""
        lines = self.send_command(f"tune set {name} {value}")
        for line in lines:
            m = self.OK_RE.match(line)
            if m:
                return {"ok": True, "name": m.group(1), "value": int(m.group(2))}
        # Check for error
        for line in lines:
            if "Error" in line or "error" in line or "$ERR" in line:
                return {"ok": False, "error": line}
        return {"ok": False, "error": "No response", "raw": lines}

    def apply_center(self) -> bool:
        lines = self.send_command("tune apply")
        return any(self.APPLIED_RE.match(l) for l in lines)

    def save_params(self) -> bool:
        """Send 'tune save' to persist params to NVS flash."""
        lines = self.send_command("tune save")
        return any("$SAVED" in l for l in lines)


# ────────────────────── FastAPI app ──────────────────────

from contextlib import asynccontextmanager

ser_mgr: SerialManager | None = None

# Valid parameter names whitelist (prevents command injection)
VALID_PARAMS = {
    "h_center", "v_center", "h_left", "h_right", "v_top", "v_bottom",
    "h_offset", "v_offset", "slew_rate", "interval", "ema_num",
    "ema_den", "fire_cooldown", "fire_pulse",
}

# Active WebSocket connections
ws_clients: set[WebSocket] = set()

# Event loop reference (set at startup, used by serial callback thread)
_event_loop: asyncio.AbstractEventLoop | None = None


async def broadcast(msg: dict) -> None:
    """Send JSON to all connected WebSocket clients."""
    data = json.dumps(msg)
    dead = set()
    for ws in ws_clients:
        try:
            await ws.send_text(data)
        except Exception:
            dead.add(ws)
    ws_clients -= dead


def on_telem(telem: dict) -> None:
    """Callback from serial reader thread → broadcast to WebSocket clients."""
    if _event_loop and _event_loop.is_running():
        asyncio.run_coroutine_threadsafe(broadcast(telem), _event_loop)


@asynccontextmanager
async def lifespan(application: FastAPI):
    global _event_loop
    _event_loop = asyncio.get_running_loop()
    if ser_mgr:
        ser_mgr.add_telem_callback(on_telem)
    yield


app = FastAPI(title="CyberPiercer Tuner", lifespan=lifespan)


@app.get("/", response_class=HTMLResponse)
async def index():
    """Serve the tuner HTML UI."""
    html_path = Path(__file__).parent / "tuner.html"
    if html_path.exists():
        return HTMLResponse(html_path.read_text(encoding="utf-8"))
    return HTMLResponse("<h1>tuner.html not found</h1>")


@app.get("/api/params")
async def api_get_params():
    """Get all current parameters."""
    if not ser_mgr:
        return {"error": "Serial not connected"}
    return await asyncio.get_running_loop().run_in_executor(
        None, ser_mgr.get_all_params
    )


@app.post("/api/param")
async def api_set_param(body: dict):
    """Set a single parameter. Body: {"name": "h_center", "value": 1722}"""
    if not ser_mgr:
        return {"error": "Serial not connected"}
    name = body.get("name", "")
    value = body.get("value")
    if not name or value is None:
        return {"error": "Missing name or value"}
    if name not in VALID_PARAMS:
        return {"error": f"Invalid param name: {name}"}
    return await asyncio.get_running_loop().run_in_executor(
        None, ser_mgr.set_param, name, int(value)
    )


@app.post("/api/apply")
async def api_apply():
    """Apply center values to servos."""
    if not ser_mgr:
        return {"error": "Serial not connected"}
    ok = await asyncio.get_running_loop().run_in_executor(
        None, ser_mgr.apply_center
    )
    return {"ok": ok}


@app.post("/api/save")
async def api_save():
    """Save all params to NVS flash."""
    if not ser_mgr:
        return {"error": "Serial not connected"}
    ok = await asyncio.get_running_loop().run_in_executor(
        None, ser_mgr.save_params
    )
    return {"ok": ok}


@app.get("/api/ports")
async def api_list_ports():
    """List available serial ports."""
    from serial.tools.list_ports import comports
    ports = [{"device": p.device, "desc": p.description} for p in comports()]
    current = ser_mgr.port if ser_mgr else None
    connected = ser_mgr.connected if ser_mgr else False
    return {"ports": ports, "current": current, "connected": connected}


@app.post("/api/connect")
async def api_connect(body: dict):
    """Connect or reconnect to a serial port. Body: {"port": "COM5"}"""
    global ser_mgr
    port = body.get("port", "")
    if not port:
        return {"ok": False, "error": "Missing port"}
    loop = asyncio.get_running_loop()
    try:
        if ser_mgr:
            await loop.run_in_executor(None, ser_mgr.reconnect, port)
        else:
            ser_mgr = SerialManager(port, 115200)
            await loop.run_in_executor(None, ser_mgr.connect)
            ser_mgr.add_telem_callback(on_telem)
        return {"ok": True, "port": port}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/disconnect")
async def api_disconnect():
    """Disconnect serial port."""
    global ser_mgr
    if ser_mgr:
        ser_mgr.close()
    return {"ok": True}


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    """WebSocket for real-time telemetry streaming."""
    global ser_mgr
    await ws.accept()
    ws_clients.add(ws)
    log.info("WebSocket client connected (%d total)", len(ws_clients))
    try:
        while True:
            data = await ws.receive_text()
            # Client can send commands via WebSocket too
            try:
                msg = json.loads(data)
                loop = asyncio.get_running_loop()
                if msg.get("cmd") == "get_params":
                    params = await loop.run_in_executor(
                        None, ser_mgr.get_all_params) if ser_mgr else {}
                    await ws.send_text(json.dumps({"type": "params", "data": params}))
                elif msg.get("cmd") == "set_param":
                    name = msg.get("name", "")
                    if name not in VALID_PARAMS:
                        await ws.send_text(json.dumps({"type": "set_result", "data": {"ok": False, "error": f"Invalid param: {name}"}}))
                    elif ser_mgr:
                        result = await loop.run_in_executor(
                            None, ser_mgr.set_param, name, int(msg["value"])
                        )
                        await ws.send_text(json.dumps({"type": "set_result", "data": result}))
                    else:
                        await ws.send_text(json.dumps({"type": "set_result", "data": {"error": "no serial"}}))
                elif msg.get("cmd") == "apply":
                    ok = await loop.run_in_executor(
                        None, ser_mgr.apply_center) if ser_mgr else False
                    await ws.send_text(json.dumps({"type": "apply_result", "ok": ok}))
                elif msg.get("cmd") == "telem":
                    if ser_mgr:
                        on_off = msg.get("enabled", True)
                        await loop.run_in_executor(
                            None, ser_mgr.send_command,
                            f"tune telem {'on' if on_off else 'off'}")
                        await ws.send_text(json.dumps({"type": "telem_ctrl", "enabled": on_off}))
                elif msg.get("cmd") == "save":
                    ok = await loop.run_in_executor(
                        None, ser_mgr.save_params) if ser_mgr else False
                    await ws.send_text(json.dumps({"type": "save_result", "ok": ok}))
                elif msg.get("cmd") == "connect":
                    port = msg.get("port", "")
                    if not port:
                        await ws.send_text(json.dumps({"type": "connect_result", "ok": False, "error": "Missing port"}))
                    else:
                        try:
                            if ser_mgr:
                                await loop.run_in_executor(None, ser_mgr.reconnect, port)
                            else:
                                ser_mgr = SerialManager(port, 115200)
                                await loop.run_in_executor(None, ser_mgr.connect)
                                ser_mgr.add_telem_callback(on_telem)
                            await ws.send_text(json.dumps({"type": "connect_result", "ok": True, "port": port}))
                        except Exception as e:
                            await ws.send_text(json.dumps({"type": "connect_result", "ok": False, "error": str(e)}))
                elif msg.get("cmd") == "disconnect":
                    if ser_mgr:
                        await loop.run_in_executor(None, ser_mgr.close)
                    await ws.send_text(json.dumps({"type": "connect_result", "ok": True, "port": "", "connected": False}))
                elif msg.get("cmd") == "get_ports":
                    from serial.tools.list_ports import comports
                    ports = [{"device": p.device, "desc": p.description} for p in comports()]
                    current = ser_mgr.port if ser_mgr else None
                    connected = ser_mgr.connected if ser_mgr else False
                    await ws.send_text(json.dumps({"type": "ports", "ports": ports, "current": current, "connected": connected}))
            except (json.JSONDecodeError, ValueError, KeyError, TypeError):
                pass
    except WebSocketDisconnect:
        pass
    finally:
        ws_clients.discard(ws)
        log.info("WebSocket client disconnected (%d remaining)", len(ws_clients))


# ────────────────────── Main ──────────────────────

def main():
    global ser_mgr

    parser = argparse.ArgumentParser(description="CyberPiercer Tuner Server")
    parser.add_argument("--port", default="COM5", help="Serial port (default: COM5)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--web-port", type=int, default=9000, help="Web server port")
    parser.add_argument("--camera-url", default="http://localhost:8080/mjpeg",
                        help="Camera MJPEG stream URL")
    args = parser.parse_args()

    # Store camera URL for frontend
    os.environ["TUNER_CAMERA_URL"] = args.camera_url

    ser_mgr = SerialManager(args.port, args.baud)
    try:
        ser_mgr.connect()
    except serial.SerialException as e:
        log.error("Cannot open serial port %s: %s", args.port, e)
        log.info("Starting without serial connection (web UI only)")
        ser_mgr = None

    log.info("Starting tuner web server on http://localhost:%d", args.web_port)
    uvicorn.run(app, host="0.0.0.0", port=args.web_port, log_level="info")


if __name__ == "__main__":
    main()
