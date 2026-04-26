# smart_camera PC server

USB camera lives here. ESP32-S3 LCD is just a remote viewport over Wi-Fi.

## Phase 2 (current): plain MJPEG passthrough

```powershell
cd samples/boards/espressif/esp32-s3-box/smart_camera/pc_server
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt

# Use device 0 (first USB cam). For your 5MP cam, hint capture size:
python server.py --device 0 --src-width 2944 --src-height 1656 --port 8080
```

Verify in a browser on the same machine:

* `http://localhost:8080/`        — landing page with embedded preview
* `http://localhost:8080/mjpeg`   — raw multipart MJPEG stream (320x240)
* `http://localhost:8080/status`  — JSON

You should see the camera feed with a green `#<frame>` counter overlay in
the top-left corner. Console logs an FPS line every 5 s.

## Coming in later phases

* Phase 4: face / object recognition with InsightFace + YOLOv8 (boxes drawn into the JPEG before encoding).
* Phase 5: `POST /mode {face|object|none}` endpoint, called by ESP32 buttons.

## Notes

* Windows: the server uses `cv2.CAP_DSHOW` for better USB-cam compat. Linux/macOS fall back to the default backend.
* Output is always resized to **320x240** (LCD native size) before encoding — recognition will run on the full-resolution capture in Phase 4.
* JPEG quality defaults to 70; tune if bandwidth is tight.
