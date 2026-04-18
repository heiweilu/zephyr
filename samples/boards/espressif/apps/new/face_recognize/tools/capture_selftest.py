"""Capture serial log until JPEG_END marker. Robust to ESP32-S3 USB-CDC
reset re-enumeration: keeps retrying open() and read().

Usage:
    python capture_selftest.py <COM_PORT> [output_file]

Steps:
    1. Run this script.
    2. While it prints "waiting for port ...", press the board's RST/EN button.
    3. Script reconnects after re-enumeration, captures until JPEG_END, exits.
"""
import sys
import time

try:
    import serial
except ImportError:
    print("pip install pyserial first", file=sys.stderr)
    sys.exit(1)

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
out_path = sys.argv[2] if len(sys.argv) > 2 else "capture.log"
baud = 115200
total_timeout_s = 60

print(f"Target: {port} @ {baud}, output: {out_path}")
print(">>> When ready: press BOOT (snapshot) or RST (selftest) on the board. <<<")

start = time.time()
got_begin = False
got_end = False

with open(out_path, "wb") as f:
    while time.time() - start < total_timeout_s and not got_end:
        ser = None
        while ser is None and time.time() - start < total_timeout_s:
            try:
                ser = serial.Serial(port, baud, timeout=1)
                print(f"[opened {port}]")
            except (serial.SerialException, OSError) as e:
                print(f"  waiting for port ... ({e})")
                time.sleep(0.5)
        if ser is None:
            break

        try:
            while time.time() - start < total_timeout_s and not got_end:
                try:
                    chunk = ser.read(4096)
                except (serial.SerialException, OSError) as e:
                    print(f"\n[read failed: {e}, will reconnect]")
                    break
                if chunk:
                    f.write(chunk)
                    f.flush()
                    sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                    if b"JPEG_BEGIN" in chunk:
                        got_begin = True
                    if got_begin and b"JPEG_END" in chunk:
                        got_end = True
                        break
        finally:
            try:
                ser.close()
            except Exception:
                pass

if got_end:
    print(f"\n[OK: captured to {out_path}]")
    sys.exit(0)
else:
    print(f"\n[TIMEOUT: no JPEG_END within {total_timeout_s}s]")
    print("Tip: selftest runs only ONCE at boot. Press RST while script is waiting.")
    sys.exit(1)
