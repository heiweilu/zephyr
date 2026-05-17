"""Color-based balloon detector using HSV filtering.

Detects red and yellow colored regions in the frame and returns bounding boxes.
"""
from __future__ import annotations

from typing import List, Tuple

import cv2
import numpy as np

BBox = Tuple[int, int, int, int]  # x1, y1, x2, y2

# HSV ranges for target colors.
# Red wraps around H=0/180, so we use two ranges.
_RED_LOWER1 = np.array([0, 100, 100], dtype=np.uint8)
_RED_UPPER1 = np.array([10, 255, 255], dtype=np.uint8)
_RED_LOWER2 = np.array([170, 100, 100], dtype=np.uint8)
_RED_UPPER2 = np.array([180, 255, 255], dtype=np.uint8)

_YELLOW_LOWER = np.array([20, 100, 100], dtype=np.uint8)
_YELLOW_UPPER = np.array([35, 255, 255], dtype=np.uint8)

# Minimum contour area (in pixels) to filter noise.
_MIN_AREA = 500

# Maximum contour area ratio — reject regions larger than 60% of frame.
_MAX_AREA_RATIO = 0.6

# Morphological kernel for noise removal.
_KERNEL = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))


class ColorDetector:
    """Detect red and yellow balloons via HSV color filtering."""

    def detect(self, bgr: np.ndarray) -> List[BBox]:
        if bgr is None or bgr.size == 0:
            return []

        hsv = cv2.cvtColor(bgr, cv2.COLOR_BGR2HSV)

        # Red mask (two ranges due to hue wrap-around).
        mask_r1 = cv2.inRange(hsv, _RED_LOWER1, _RED_UPPER1)
        mask_r2 = cv2.inRange(hsv, _RED_LOWER2, _RED_UPPER2)
        mask_red = mask_r1 | mask_r2

        # Yellow mask.
        mask_yellow = cv2.inRange(hsv, _YELLOW_LOWER, _YELLOW_UPPER)

        # Combined mask.
        mask = mask_red | mask_yellow

        # Morphological open to remove small noise, close to fill holes.
        mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, _KERNEL)
        mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, _KERNEL)

        # Find contours.
        contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL,
                                       cv2.CHAIN_APPROX_SIMPLE)

        frame_area = bgr.shape[0] * bgr.shape[1]
        max_area = frame_area * _MAX_AREA_RATIO

        boxes: List[BBox] = []
        for cnt in contours:
            area = cv2.contourArea(cnt)
            if area < _MIN_AREA or area > max_area:
                continue
            x, y, w, h = cv2.boundingRect(cnt)
            boxes.append((x, y, x + w, y + h))

        return boxes
