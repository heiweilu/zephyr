"""Face detector using OpenCV YuNet (cv2.FaceDetectorYN_create).

Model file 'face_detection_yunet_2023mar.onnx' (~336 KB) is auto-downloaded
on first call from the official OpenCV Zoo into ./models/.
"""
from __future__ import annotations

import logging
import os
import threading
from typing import List, Tuple

import cv2
import numpy as np

BBox = Tuple[int, int, int, int]

MODEL_URL = (
    "https://github.com/opencv/opencv_zoo/raw/main/"
    "models/face_detection_yunet/face_detection_yunet_2023mar.onnx"
)
MODEL_PATH = os.path.join(os.path.dirname(__file__), "models",
                          "face_detection_yunet_2023mar.onnx")


def _download_model() -> bool:
    try:
        import requests
        os.makedirs(os.path.dirname(MODEL_PATH), exist_ok=True)
        logging.info("downloading YuNet model -> %s", MODEL_PATH)
        r = requests.get(MODEL_URL, timeout=60)
        r.raise_for_status()
        with open(MODEL_PATH, "wb") as fp:
            fp.write(r.content)
        return True
    except Exception as exc:
        logging.warning("YuNet download failed: %s", exc)
        return False


class FaceDetector:
    def __init__(self, score: float = 0.6) -> None:
        self._score = score
        self._det = None
        self._failed = False
        self._lock = threading.Lock()
        self._cur_size = (0, 0)

    def _ensure_loaded(self, w: int, h: int) -> bool:
        if self._failed:
            return False
        with self._lock:
            if self._det is None:
                if not os.path.exists(MODEL_PATH) and not _download_model():
                    self._failed = True
                    return False
                try:
                    self._det = cv2.FaceDetectorYN_create(
                        MODEL_PATH, "", (w, h),
                        score_threshold=self._score)
                    self._cur_size = (w, h)
                    logging.info("YuNet ready (%dx%d)", w, h)
                except Exception as exc:
                    logging.warning("YuNet init failed: %s", exc)
                    self._failed = True
                    return False
            elif self._cur_size != (w, h):
                self._det.setInputSize((w, h))
                self._cur_size = (w, h)
            return True

    def detect(self, bgr: np.ndarray) -> List[BBox]:
        h, w = bgr.shape[:2]
        if not self._ensure_loaded(w, h):
            return []
        try:
            _, faces = self._det.detect(bgr)
        except Exception as exc:
            logging.warning("YuNet detect failed: %s", exc)
            return []
        out: List[BBox] = []
        if faces is None:
            return out
        for face in faces:
            x, y, fw, fh = (int(v) for v in face[:4])
            out.append((x, y, x + fw, y + fh))
        return out