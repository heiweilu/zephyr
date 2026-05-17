"""
smart_camera PC server — Phase 2: minimal MJPEG-over-HTTP.

USB camera in -> OpenCV resize to 320x240 -> JPEG encode ->
HTTP multipart/x-mixed-replace stream on /mjpeg.

Run:
    python server.py --device 0 --port 8080
Open http://<this-pc-ip>:8080/mjpeg in a browser to verify.

Phase 4 will add face/object recognition; the streaming code stays the same.
"""
from __future__ import annotations

import argparse
import logging
import os
import subprocess
import threading
import time

# Workaround: OpenCV 4.13 on Windows may fail to open USB cameras with the
# default MSMF backend. Force DirectShow by lowering MSMF priority.
os.environ.setdefault("OPENCV_VIDEOIO_PRIORITY_MSMF", "0")

import cv2
import numpy as np
from fastapi import FastAPI, HTTPException
from fastapi.responses import HTMLResponse, StreamingResponse
from pydantic import BaseModel
import uvicorn

from color import ColorDetector
from face import FaceDetector
from object import ObjectDetector

# Output frame dims = ESP32 LCD resolution.
OUT_W, OUT_H = 320, 240
JPEG_QUALITY = 25  # very low quality -> ~3-4KB/frame, much shorter Wi-Fi airtime
MAX_STREAM_FPS = 7   # match ESP decode rate; higher values back up TCP and add huge latency


class CameraGrabber:
    """Background thread that always holds the latest BGR frame from the cam.

    Avoids backpressure: HTTP clients always get the freshest frame, never queue.
    """

    def __init__(self, device: int, src_w: int | None, src_h: int | None,
                 device_name: str | None = None) -> None:
        self.device = device
        self.device_name = device_name
        self.src_w = src_w
        self.src_h = src_h
        self._cap: cv2.VideoCapture | None = None
        self._ffmpeg_proc: subprocess.Popen | None = None
        self._frame_w: int = src_w or 640
        self._frame_h: int = src_h or 480
        self._latest: np.ndarray | None = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        # On Windows, CAP_DSHOW gives much better USB-cam compat than the default backend.
        self._cap = cv2.VideoCapture(self.device, cv2.CAP_DSHOW)
        if not self._cap.isOpened():
            # Fallback to default backend (Linux / macOS).
            self._cap = cv2.VideoCapture(self.device)
        if not self._cap.isOpened():
            self._cap.release()
            self._cap = None
            # Fallback: use ffmpeg subprocess via DirectShow (works when OpenCV can't enumerate).
            name = self.device_name or self._discover_camera_name()
            if name:
                self._start_ffmpeg(name)
            else:
                raise RuntimeError(f"Cannot open camera device {self.device}")
        else:
            if self.src_w:
                self._cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.src_w)
            if self.src_h:
                self._cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.src_h)
            self._frame_w = int(self._cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            self._frame_h = int(self._cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            logging.info("camera %d opened (cv2), capture size %dx%d",
                         self.device, self._frame_w, self._frame_h)

        self._thread = threading.Thread(target=self._loop, name="cam-grab", daemon=True)
        self._thread.start()

    def _discover_camera_name(self) -> str | None:
        """Try ffmpeg -list_devices to find the first video device name."""
        try:
            result = subprocess.run(
                ["ffmpeg", "-list_devices", "true", "-f", "dshow", "-i", "dummy"],
                capture_output=True, timeout=5)
            # Device names appear in stderr as: "DeviceName" (video)
            stderr_text = result.stderr.decode("utf-8", errors="replace")
            for line in stderr_text.splitlines():
                if "(video)" in line:
                    # Extract quoted name
                    start = line.find('"')
                    end = line.find('"', start + 1)
                    if start >= 0 and end > start:
                        name = line[start + 1:end]
                        logging.info("discovered camera via ffmpeg: %s", name)
                        return name
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass
        return None

    def _start_ffmpeg(self, name: str) -> None:
        """Start ffmpeg subprocess to capture from DirectShow device."""
        cmd = [
            "ffmpeg", "-f", "dshow",
            "-vcodec", "mjpeg",
            "-video_size", f"{self._frame_w}x{self._frame_h}",
            "-framerate", "30",
            "-i", f"video={name}",
            "-pix_fmt", "bgr24",
            "-f", "rawvideo",
            "-loglevel", "error",
            "-"
        ]
        self._ffmpeg_proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        logging.info("camera opened via ffmpeg (dshow): %s, %dx%d",
                     name, self._frame_w, self._frame_h)

    def _loop(self) -> None:
        if self._ffmpeg_proc is not None:
            self._loop_ffmpeg()
        else:
            self._loop_cv2()

    def _loop_cv2(self) -> None:
        assert self._cap is not None
        while not self._stop.is_set():
            ok, frame = self._cap.read()
            if not ok or frame is None:
                time.sleep(0.01)
                continue
            with self._lock:
                self._latest = frame

    def _loop_ffmpeg(self) -> None:
        assert self._ffmpeg_proc is not None
        frame_size = self._frame_w * self._frame_h * 3
        while not self._stop.is_set():
            raw = self._ffmpeg_proc.stdout.read(frame_size)
            if len(raw) != frame_size:
                time.sleep(0.01)
                continue
            frame = np.frombuffer(raw, dtype=np.uint8).reshape(
                self._frame_h, self._frame_w, 3)
            with self._lock:
                self._latest = frame

    def get_latest(self) -> np.ndarray | None:
        with self._lock:
            return None if self._latest is None else self._latest.copy()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
        if self._cap is not None:
            self._cap.release()
        if self._ffmpeg_proc is not None:
            self._ffmpeg_proc.terminate()
            self._ffmpeg_proc.wait(timeout=2.0)


def _draw_crosshair(img, cx: int, cy: int, color) -> None:
    """Animated-looking crosshair: outer ring + inner cross."""
    cv2.circle(img, (cx, cy), 22, color, 1, cv2.LINE_AA)
    cv2.circle(img, (cx, cy), 4, color, -1, cv2.LINE_AA)
    cv2.line(img, (cx - 30, cy), (cx - 8, cy), color, 2, cv2.LINE_AA)
    cv2.line(img, (cx + 8, cy), (cx + 30, cy), color, 2, cv2.LINE_AA)
    cv2.line(img, (cx, cy - 30), (cx, cy - 8), color, 2, cv2.LINE_AA)
    cv2.line(img, (cx, cy + 8), (cx, cy + 30), color, 2, cv2.LINE_AA)


# Latest detection target in OUTPUT-frame coords (320x240).
# (cx, cy, seq) or None if no target.
_target_lock = threading.Lock()
_target: tuple[int, int, int] | None = None
_target_seq = 0


def _publish_target(cx: int | None, cy: int | None) -> None:
    global _target, _target_seq
    with _target_lock:
        _target_seq += 1
        if cx is None:
            _target = None
        else:
            _target = (int(cx), int(cy), _target_seq)


def _get_target() -> tuple[int, int, int] | None:
    with _target_lock:
        return _target


def _detection_loop() -> None:
    """Run detector on the latest cam frame as fast as possible; publish
    target center scaled to OUT_W/OUT_H."""
    while True:
        if camera is None:
            time.sleep(0.05)
            continue
        frame = camera.get_latest()
        if frame is None:
            time.sleep(0.01)
            continue

        mode = current_mode()
        target_box = None

        if mode == "face":
            boxes = face_detector.detect(frame)
            if boxes:
                target_box = max(boxes,
                                 key=lambda b: (b[2] - b[0]) * (b[3] - b[1]))
        elif mode == "object":
            dets = object_detector.detect(frame)
            best = None
            best_conf = 0.0
            for x1, y1, x2, y2, _label, conf in dets:
                if conf > best_conf:
                    best_conf = conf
                    best = (x1, y1, x2, y2)
            target_box = best
        elif mode == "color":
            boxes = color_detector.detect(frame)
            if boxes:
                # Pick the balloon closest to the ground (highest y2).
                target_box = max(boxes, key=lambda b: b[3])

        if target_box is None:
            _publish_target(None, None)
        else:
            src_h, src_w = frame.shape[:2]
            cx_src = (target_box[0] + target_box[2]) // 2
            cy_src = (target_box[1] + target_box[3]) // 2
            cx = int(cx_src * OUT_W / src_w)
            cy = int(cy_src * OUT_H / src_h)
            _publish_target(cx, cy)


def render_for_esp32(frame: np.ndarray, frame_no: int) -> bytes:
    """Resize + encode. Draw crosshair on current target if any."""
    out = cv2.resize(frame, (OUT_W, OUT_H), interpolation=cv2.INTER_AREA)
    tgt = _get_target()
    if tgt is not None:
        _draw_crosshair(out, tgt[0], tgt[1], (0, 0, 255))
    cv2.putText(out, f"#{frame_no} {current_mode()}", (4, 18),
                cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 255, 0), 2, cv2.LINE_AA)
    ok, buf = cv2.imencode(".jpg", out, [int(cv2.IMWRITE_JPEG_QUALITY), JPEG_QUALITY])
    if not ok:
        raise RuntimeError("JPEG encode failed")
    return buf.tobytes()


# ─── FastAPI app ────────────────────────────────────────────────────────────

app = FastAPI(title="smart_camera PC server")
camera: CameraGrabber | None = None
color_detector = ColorDetector()
face_detector = FaceDetector()
object_detector = ObjectDetector()

VALID_MODES = ("raw", "face", "object", "color")
_mode_lock = threading.Lock()
_mode = "raw"


def current_mode() -> str:
    with _mode_lock:
        return _mode


def set_mode(m: str) -> None:
    global _mode
    with _mode_lock:
        _mode = m


@app.get("/", response_class=HTMLResponse)
def index() -> str:
    return (
        "<html><body style='background:#222;color:#eee;font-family:sans-serif'>"
        "<h2>smart_camera PC server</h2>"
        "<p>Endpoints:</p><ul>"
        "<li><a href='/mjpeg' style='color:#9ee493'>/mjpeg</a> — multipart MJPEG stream (320x240)</li>"
        "<li><code>/status</code> — JSON state</li>"
        "</ul>"
        "<img src='/mjpeg' style='border:1px solid #555' />"
        "</body></html>"
    )


@app.get("/status")
def status() -> dict:
    return {
        "camera_open": camera is not None,
        "capture_w": camera._frame_w if camera else 0,
        "capture_h": camera._frame_h if camera else 0,
        "out_w": OUT_W,
        "out_h": OUT_H,
        "mode": current_mode(),
        "valid_modes": list(VALID_MODES),
    }


class ModeReq(BaseModel):
    mode: str


@app.post("/mode")
def post_mode(req: ModeReq) -> dict:
    if req.mode not in VALID_MODES:
        raise HTTPException(status_code=400,
                            detail=f"mode must be one of {VALID_MODES}")
    set_mode(req.mode)
    logging.info("mode -> %s", req.mode)
    return {"mode": req.mode}


def _track_stream():
    """NDJSON stream of {seq, cx, cy} or {seq, cx: -1} when no target.
    20 Hz heartbeat so the ESP keeps the connection alive even with no target.
    """
    last_seq = -1
    while True:
        t = _get_target()
        if t is not None:
            cx, cy, seq = t
        else:
            with _target_lock:
                seq = _target_seq
            cx, cy = -1, -1
        if seq != last_seq:
            last_seq = seq
            yield f'{{"seq":{seq},"cx":{cx},"cy":{cy}}}\n'.encode()
        time.sleep(0.05)  # 20 Hz


@app.get("/track")
def track() -> StreamingResponse:
    return StreamingResponse(_track_stream(), media_type="application/x-ndjson")


def mjpeg_stream():
    boundary = b"--frame"
    frame_no = 0
    last_log = time.time()
    last_count = 0
    min_interval = 1.0 / MAX_STREAM_FPS
    next_emit = time.time()
    while True:
        if camera is None:
            time.sleep(0.1)
            continue
        # Pace the stream to MAX_STREAM_FPS to avoid backing up the TCP
        # send buffer, which adds visible end-to-end latency.
        now = time.time()
        if now < next_emit:
            time.sleep(next_emit - now)
        next_emit = max(now, next_emit) + min_interval

        frame = camera.get_latest()
        if frame is None:
            time.sleep(0.01)
            continue
        try:
            jpg = render_for_esp32(frame, frame_no)
        except Exception as exc:
            logging.exception("encode failed: %s", exc)
            time.sleep(0.05)
            continue
        frame_no += 1

        now = time.time()
        if now - last_log >= 5.0:
            fps = (frame_no - last_count) / (now - last_log)
            logging.info("served %d frames, %.1f FPS", frame_no, fps)
            last_log = now
            last_count = frame_no

        yield (
            boundary + b"\r\n"
            b"Content-Type: image/jpeg\r\n"
            b"Content-Length: " + str(len(jpg)).encode() + b"\r\n\r\n"
            + jpg + b"\r\n"
        )


@app.get("/mjpeg")
def mjpeg() -> StreamingResponse:
    return StreamingResponse(
        mjpeg_stream(),
        media_type="multipart/x-mixed-replace; boundary=frame",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", type=int, default=0,
                        help="OpenCV camera index (0 is usually the first USB cam)")
    parser.add_argument("--name", type=str, default=None,
                        help="Camera device name for ffmpeg fallback (e.g. '5MP USB Camera')")
    parser.add_argument("--src-width", type=int, default=None,
                        help="Capture width hint (e.g. 2944 for 5MP)")
    parser.add_argument("--src-height", type=int, default=None,
                        help="Capture height hint (e.g. 1656 for 5MP)")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s [%(levelname)s] %(message)s")

    global camera
    camera = CameraGrabber(args.device, args.src_width, args.src_height,
                           device_name=args.name)
    camera.start()
    threading.Thread(target=_detection_loop, name="det", daemon=True).start()
    try:
        uvicorn.run(app, host=args.host, port=args.port, log_level="warning")
    finally:
        camera.stop()


if __name__ == "__main__":
    main()
