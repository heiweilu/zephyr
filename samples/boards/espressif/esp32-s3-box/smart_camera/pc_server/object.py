"""Object detector wrapper using YOLOv8n via ultralytics.

Lazy-loaded; model file 'yolov8n.pt' is auto-downloaded by ultralytics on
first use (~6 MB). On load failure detect() returns [] and logs once.
"""
from __future__ import annotations

import logging
import threading
from typing import List, Tuple

import numpy as np


Detection = Tuple[int, int, int, int, str, float]  # x1,y1,x2,y2,label,conf


class ObjectDetector:
    def __init__(self, conf: float = 0.35) -> None:
        self._conf = conf
        self._model = None
        self._failed = False
        self._lock = threading.Lock()
        self._names: dict = {}

    def _ensure_loaded(self) -> bool:
        if self._model is not None:
            return True
        if self._failed:
            return False
        with self._lock:
            if self._model is not None:
                return True
            if self._failed:
                return False
            try:
                from ultralytics import YOLO
                m = YOLO("yolov8n.pt")
                self._model = m
                self._names = m.names if hasattr(m, "names") else {}
                logging.info("YOLOv8n ready (%d classes)", len(self._names))
                return True
            except Exception as exc:
                logging.warning("yolo load failed: %s", exc)
                self._failed = True
                return False

    def detect(self, bgr: np.ndarray) -> List[Detection]:
        if not self._ensure_loaded():
            return []
        try:
            results = self._model.predict(bgr, conf=self._conf, verbose=False)
        except Exception as exc:
            logging.warning("yolo predict failed: %s", exc)
            return []
        out: List[Detection] = []
        for r in results:
            boxes = getattr(r, "boxes", None)
            if boxes is None:
                continue
            for b in boxes:
                xyxy = b.xyxy[0].tolist()
                x1, y1, x2, y2 = (int(v) for v in xyxy)
                cls_id = int(b.cls[0].item()) if b.cls is not None else -1
                conf = float(b.conf[0].item()) if b.conf is not None else 0.0
                label = self._names.get(cls_id, str(cls_id))
                out.append((x1, y1, x2, y2, label, conf))
        return out
