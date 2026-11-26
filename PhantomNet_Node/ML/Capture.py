#!/usr/bin/env python3
"""
PHANTOM NET - data capture recorder
===================================

Reads the IMU stream from PhantomNet_DataCapture.ino and saves it to CSV,
while letting you change the activity label from the keyboard as you go.

Why this instead of PuTTY or the Arduino Serial Monitor:
  * the Arduino Serial Monitor cannot save to a file at all
  * you need to change the label WHILE wearing the helmet, and a live
    per-label row count tells you when you have collected enough of each
  * it counts your samples so you stop guessing

INSTALL
    pip install pyserial

RUN
    python capture.py COM5 rishi_session1

CONTROLS (press the key, no Enter needed)
    0-9   set the activity label
    s     start / stop streaming
    q     quit and save

The output file is capture_<name>.csv, ready for train_fall.py.
"""

import sys
import os
import time
import csv

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing.  Run:  pip install pyserial")

# Non-blocking single-key input. Windows uses msvcrt; Linux/macOS use termios.
if os.name == "nt":
    import msvcrt

    def key_pressed():
        return msvcrt.getch().decode("utf-8", "ignore") if msvcrt.kbhit() else None
else:
    import select
    import termios
    import tty

    def key_pressed():
        if select.select([sys.stdin], [], [], 0)[0]:
            return sys.stdin.read(1)
        return None


LABELS = {
    0: "still",         1: "walking",      2: "running",
    3: "head_turn",     4: "bend_pickup",  5: "sit_stand",
    6: "helmet_on_off", 7: "FALL_forward", 8: "FALL_backward",
    9: "FALL_side",
}

# Rough guidance on how much of each you need. Falls are what you have
# least of and need most, so they get the same target despite being the
# most effort to collect.
TARGET = {0: 2000, 1: 4000, 2: 2000, 3: 3000, 4: 3000,
          5: 3000, 6: 3000, 7: 2000, 8: 2000, 9: 2000}


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: python capture.py <PORT> <session_name>\n"
                 "  e.g. python capture.py COM5 rishi_session1")

    port, name = sys.argv[1], sys.argv[2]
    out_path = f"capture_{name}.csv"

    if os.path.exists(out_path):
        if input(f"{out_path} exists. Overwrite? [y/N] ").lower() != "y":
            sys.exit("cancelled")

    try:
        ser = serial.Serial(port, 921600, timeout=0.1)
    except serial.SerialException as e:
        sys.exit(f"cannot open {port}: {e}\n"
                 "Close the Arduino Serial Monitor first - only one program "
                 "can hold a COM port.")

    time.sleep(2)                 # let the ESP32 finish booting
    ser.reset_input_buffer()

    counts = {k: 0 for k in LABELS}
    label = 0
    rows = 0
    header_written = False
    last_report = 0.0

    print(f"\nRecording to {out_path}")
    print("Press 's' on the ESP32 to start streaming, 'q' here to finish.\n")

    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        try:
            while True:
                # ---- keyboard: forward the keystroke to the ESP32 --------
                k = key_pressed()
                if k:
                    if k == "q":
                        break
                    if k.isdigit():
                        label = int(k)
                        print(f"\n  LABEL -> {label} {LABELS[label]}")
                    ser.write(k.encode())

                # ---- serial: read one line -------------------------------
                raw = ser.readline().decode("utf-8", "ignore").strip()
                if not raw:
                    continue

                if raw.startswith("#"):          # commentary from the sketch
                    print("  " + raw)
                    continue

                if raw.startswith("t_ms"):       # header line
                    if not header_written:
                        writer.writerow(raw.split(","))
                        header_written = True
                    continue

                parts = raw.split(",")
                if len(parts) != 9:              # partial line, skip it
                    continue

                try:
                    lbl = int(parts[1])
                except ValueError:
                    continue

                writer.writerow(parts)
                rows += 1
                counts[lbl] = counts.get(lbl, 0) + 1

                # ---- progress, once a second -----------------------------
                now = time.time()
                if now - last_report > 1.0:
                    last_report = now
                    cur = counts.get(label, 0)
                    tgt = TARGET.get(label, 2000)
                    bar = min(int(20 * cur / tgt), 20)
                    print(f"\r  [{label} {LABELS[label]:<14}] "
                          f"{'#' * bar}{'.' * (20 - bar)} "
                          f"{cur}/{tgt}   total {rows}   ",
                          end="", flush=True)

        except KeyboardInterrupt:
            pass
        finally:
            ser.close()

    # ---- summary -------------------------------------------------------
    print("\n\n" + "=" * 54)
    print(f"  saved {rows} rows to {out_path}")
    print("=" * 54)
    print(f"  {'label':<18}{'rows':>8}{'seconds':>10}{'target':>10}")
    print("  " + "-" * 44)
    short = []
    for k in sorted(LABELS):
        c = counts.get(k, 0)
        secs = c / 200.0                       # captured at 200 Hz
        flag = "" if c >= TARGET[k] else "  <- more needed"
        if c < TARGET[k]:
            short.append(f"{k} {LABELS[k]}")
        print(f"  {k} {LABELS[k]:<16}{c:>8}{secs:>9.0f}s{TARGET[k]:>10}{flag}")

    if short:
        print("\n  Still short on: " + ", ".join(short))
        print("  Run again with a new session name and train on both files.")
    else:
        print("\n  Every class has enough. Next:  python train_fall.py")


if __name__ == "__main__":
    main()