#!/usr/bin/env python3
"""
PHANTOM NET - clean up continuously-recorded fall captures
============================================================

If your fall data was recorded in CONTINUOUS mode (long press: fall,
stand up, reposition, fall again... all in one recording) instead of
one 5-second burst per fall, every row in that file - including the
time spent standing back up and walking back to position - carries the
SAME fall label (7, 8 or 9). That in-between time is not a fall, and
training on it as if it were teaches the model the wrong thing.

WHAT THIS SCRIPT DOES
    It finds the ACTUAL fall moments inside a continuous recording by
    looking for the real physical signature - a free-fall dip followed
    shortly by an impact spike (the same thing fall.cpp looks for on
    the device itself). It keeps a short window around each real event
    and throws away everything else (the recovery/reposition time)
    rather than mislabel it.

USAGE
    python clean_continuous_falls.py capture_forward.csv capture_back.csv capture_side.csv

    Each file produces a "..._clean.csv" next to it.

AFTER RUNNING
    Move or delete the ORIGINAL continuous capture_*.csv files out of
    the folder train_fall.py reads from (or rename them so they don't
    start with "capture_"), keeping only the *_clean.csv versions.
    Otherwise the raw, badly-labelled rows get trained on too.

SANITY CHECK
    The script prints how many fall events it found. Compare that to
    how many times you actually fell in that recording - if it's way
    off (found far fewer, or way more), open the CSV and look at the
    amag column around the mismatch; either two falls happened too
    close together (merged into one event) or a stumble/knock while
    repositioning got picked up as a false event.
"""

import sys
import numpy as np
import pandas as pd

FS              = 200     # Hz, must match the capture rate
FREEFALL_G      = 0.50    # below this = weightless
FREEFALL_MIN_MS = 30      # must stay weightless at least this long
IMPACT_G        = 2.50    # above this = impact
LINK_WINDOW_MS  = 800     # impact must land within this long after free-fall
PRE_MS          = 500     # keep this much data before the free-fall starts
POST_MS         = 1500    # keep this much data after the impact peak


def ms_to_rows(ms):
    return int(FS * ms / 1000)


def find_events(amag):
    """Scan amag for (free-fall -> impact) pairs. Returns [(start, impact_idx), ...]"""
    n = len(amag)
    is_freefall = amag < FREEFALL_G
    events = []
    i = 0
    while i < n:
        if is_freefall[i]:
            start = i
            while i < n and is_freefall[i]:
                i += 1
            dur_ms = (i - start) * 1000 / FS
            if dur_ms >= FREEFALL_MIN_MS:
                win = ms_to_rows(LINK_WINDOW_MS)
                seg = amag[i:i + win]
                if len(seg) and seg.max() > IMPACT_G:
                    impact_idx = i + int(np.argmax(seg))
                    events.append((start, impact_idx))
        else:
            i += 1
    return events


def clean_one(path):
    df = pd.read_csv(path)
    if "amag" not in df.columns or "label" not in df.columns:
        print(f"  {path}: missing amag/label column, skipped")
        return

    amag = df["amag"].to_numpy()
    events = find_events(amag)

    if not events:
        print(f"  {path}: NO fall events detected - is this really a fall "
              f"recording? Check the amag column by eye.")
        return

    keep = np.zeros(len(df), dtype=bool)
    for start, impact in events:
        lo = max(0, start - ms_to_rows(PRE_MS))
        hi = min(len(df), impact + ms_to_rows(POST_MS))
        keep[lo:hi] = True

    out = df[keep].copy()
    out_path = path.replace(".csv", "_clean.csv")
    out.to_csv(out_path, index=False)

    total_s = len(df) / FS
    kept_s = keep.sum() / FS
    print(f"  {path}:")
    print(f"    found {len(events)} fall event(s)")
    print(f"    kept {keep.sum()} of {len(df)} rows "
          f"({kept_s:.1f}s of {total_s:.1f}s recorded)")
    print(f"    -> {out_path}")


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: python clean_continuous_falls.py capture_xxx.csv [more files...]")

    for path in sys.argv[1:]:
        clean_one(path)

    print("\nNext: remove/rename the ORIGINAL continuous files so train_fall.py "
          "only sees the *_clean.csv versions, then run train_fall.py as usual.")


if __name__ == "__main__":
    main()
