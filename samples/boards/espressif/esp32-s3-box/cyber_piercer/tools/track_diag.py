"""
追踪诊断工具 — 同时采集串口（ESP32）和 Server Track 数据，生成可视化分析报告。

用法:
    python track_diag.py [--port COM5] [--duration 10] [--server http://192.168.1.81:8080]

输出:
    - track_diag_<timestamp>.png  可视化波形图
    - track_diag_<timestamp>.csv  原始数据（可选进一步分析）
"""

import argparse
import csv
import re
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

import serial
import requests
import matplotlib.pyplot as plt
import numpy as np

# ─────────────────── 数据存储 ────────────────────

serial_data = []   # [(t_rel, cx, ema, pulse_us, moved)]
server_data = []   # [(t_rel, cx, cy)]
lock = threading.Lock()
t0 = None


def rel_time():
    global t0
    if t0 is None:
        t0 = time.time()
    return time.time() - t0


# ─────────────────── 串口采集线程 ────────────────────

TRACK_RE = re.compile(
    r"TRACK H: cx=(-?\d+) ema=(-?\d+) pulse=(\d+)us\s*([M.]?)"
)


def serial_reader(port: str, baud: int, stop_event: threading.Event):
    try:
        ser = serial.Serial(port, baud, timeout=0.5)
    except Exception as e:
        print(f"[SERIAL] 打开 {port} 失败: {e}")
        return
    print(f"[SERIAL] 已连接 {port}@{baud}")
    while not stop_event.is_set():
        try:
            line = ser.readline().decode("utf-8", errors="replace").strip()
        except Exception:
            continue
        if not line:
            continue
        m = TRACK_RE.search(line)
        if m:
            t = rel_time()
            cx, ema, pulse = int(m.group(1)), int(m.group(2)), int(m.group(3))
            moved = m.group(4) == "M"
            with lock:
                serial_data.append((t, cx, ema, pulse, moved))
    ser.close()


# ─────────────────── Server Track 采集线程 ────────────────────

def server_reader(url: str, stop_event: threading.Event):
    import json
    track_url = url.rstrip("/") + "/track"
    print(f"[SERVER] 连接 {track_url}")
    try:
        resp = requests.get(track_url, stream=True, timeout=5)
    except Exception as e:
        print(f"[SERVER] 连接失败: {e}")
        return
    print("[SERVER] 已连接，开始采集...")
    for raw_line in resp.iter_lines():
        if stop_event.is_set():
            break
        if not raw_line:
            continue
        try:
            obj = json.loads(raw_line)
        except Exception:
            continue
        t = rel_time()
        cx = obj.get("cx", -1)
        cy = obj.get("cy", -1)
        with lock:
            server_data.append((t, cx, cy))
    resp.close()


# ─────────────────── 可视化 ────────────────────

def generate_report(output_prefix: str):
    with lock:
        sd = list(serial_data)
        sv = list(server_data)

    if not sd and not sv:
        print("[REPORT] No data collected, skipping.")
        return

    fig, axes = plt.subplots(3, 1, figsize=(14, 10), sharex=True)
    fig.suptitle(f"Track Diagnostics  |  serial={len(sd)}  server={len(sv)}", fontsize=13)

    # === Panel 1: 原始 cx 对比（Server vs ESP32 接收） ===
    ax1 = axes[0]
    if sv:
        sv_t = [p[0] for p in sv]
        sv_cx = [p[1] for p in sv]
        ax1.plot(sv_t, sv_cx, "b-", alpha=0.6, linewidth=0.8, label="Server cx (raw)")
    if sd:
        sd_t = [p[0] for p in sd]
        sd_cx = [p[1] for p in sd]
        ax1.plot(sd_t, sd_cx, "r.", markersize=3, label="ESP32 cx (received)")
    ax1.set_ylabel("cx (pixels)")
    ax1.legend(loc="upper right")
    ax1.set_title("Panel 1: Camera cx  (Server raw vs ESP32 received)")
    ax1.grid(True, alpha=0.3)

    # === Panel 2: EMA 滤波效果 ===
    ax2 = axes[1]
    if sd:
        sd_t = [p[0] for p in sd]
        sd_cx = [p[1] for p in sd]
        sd_ema = [p[2] for p in sd]
        ax2.plot(sd_t, sd_cx, "r-", alpha=0.4, linewidth=0.8, label="cx (raw)")
        ax2.plot(sd_t, sd_ema, "g-", linewidth=1.5, label="ema_cx (filtered)")
    ax2.set_ylabel("cx (pixels)")
    ax2.legend(loc="upper right")
    ax2.set_title("Panel 2: EMA 滤波效果 (raw cx vs filtered ema_cx)")
    ax2.grid(True, alpha=0.3)

    # === Panel 3: 舵机脉宽 ===
    ax3 = axes[2]
    if sd:
        sd_t = [p[0] for p in sd]
        sd_pulse = [p[3] for p in sd]
        ax3.plot(sd_t, sd_pulse, "m-", linewidth=1.2, label="servo pulse (μs)")
        # 标注中心线
        ax3.axhline(y=1722, color="gray", linestyle="--", alpha=0.5, label="center (1722μs)")
    ax3.set_ylabel("Pulse (μs)")
    ax3.set_xlabel("Time (s)")
    ax3.legend(loc="upper right")
    ax3.set_title("Panel 3: Servo Pulse Output")
    ax3.grid(True, alpha=0.3)

    plt.tight_layout()
    png_path = f"{output_prefix}.png"
    plt.savefig(png_path, dpi=150)
    print(f"[REPORT] Chart saved: {png_path}")
    plt.close()

    # === CSV 导出 ===
    csv_path = f"{output_prefix}.csv"
    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["source", "time_s", "cx", "cy_or_ema", "pulse_us", "moved"])
        for row in sv:
            w.writerow(["server", f"{row[0]:.3f}", row[1], row[2], "", ""])
        for row in sd:
            w.writerow(["esp32", f"{row[0]:.3f}", row[1], row[2], row[3], "M" if row[4] else "."])
    print(f"[REPORT] CSV saved: {csv_path}")

    # === 统计摘要 ===
    if sd:
        pulses = np.array([p[3] for p in sd])
        cxs = np.array([p[1] for p in sd])
        emas = np.array([p[2] for p in sd])
        print("\n=== Statistics ===")
        print(f"  Duration: {sd[-1][0] - sd[0][0]:.1f}s")
        print(f"  Samples: {len(sd)}")
        print(f"  cx:    mean={cxs.mean():.1f}  std={cxs.std():.1f}  range=[{cxs.min()}, {cxs.max()}]")
        print(f"  ema:   mean={emas.mean():.1f}  std={emas.std():.1f}  range=[{emas.min()}, {emas.max()}]")
        print(f"  pulse: mean={pulses.mean():.0f}  std={pulses.std():.0f}  range=[{pulses.min()}, {pulses.max()}]us")
        # 抖动指标：相邻样本差的绝对值
        if len(pulses) > 1:
            jitter = np.abs(np.diff(pulses))
            print(f"  Pulse jitter: mean={jitter.mean():.1f}us  max={jitter.max()}us  p95={np.percentile(jitter, 95):.0f}us")
        # 有效滤波率
        if len(cxs) > 1:
            cx_std = np.std(cxs)
            ema_std = np.std(emas)
            print(f"  Filter ratio: {cx_std/max(ema_std,0.01):.2f}x (cx_std/ema_std)")


# ─────────────────── 主流程 ────────────────────

def main():
    parser = argparse.ArgumentParser(description="追踪诊断工具")
    parser.add_argument("--port", default="COM5", help="ESP32 串口")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=int, default=10, help="采集时长(秒)")
    parser.add_argument("--server", default="http://192.168.1.81:8080", help="Server URL")
    parser.add_argument("--output", default=None, help="输出文件前缀")
    args = parser.parse_args()

    if args.output is None:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        args.output = f"track_diag_{ts}"

    stop = threading.Event()

    # 启动采集线程
    t_serial = threading.Thread(target=serial_reader, args=(args.port, args.baud, stop), daemon=True)
    t_server = threading.Thread(target=server_reader, args=(args.server, stop), daemon=True)
    t_serial.start()
    t_server.start()

    print(f"[MAIN] Collecting for {args.duration}s ... (face the camera!)")
    try:
        for i in range(args.duration):
            time.sleep(1)
            with lock:
                ns, nv = len(serial_data), len(server_data)
            valid_sv = sum(1 for p in server_data if p[1] >= 0)
            print(f"  [{i+1:2d}/{args.duration}] serial={ns}  server={nv} (valid={valid_sv})")
    except KeyboardInterrupt:
        print("\n[MAIN] Interrupted")

    stop.set()
    t_serial.join(timeout=2)
    t_server.join(timeout=2)

    print(f"[MAIN] Done: serial={len(serial_data)}  server={len(server_data)}")
    generate_report(args.output)


if __name__ == "__main__":
    main()
