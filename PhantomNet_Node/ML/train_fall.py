#!/usr/bin/env python3
"""
PHANTOM NET - fall classifier training
======================================

Turns the raw IMU captures from PhantomNet_DataCapture.ino into a trained
classifier, and exports it as plain C that runs on the ESP32 in microseconds.

WHAT IT DOES, IN ORDER
    1. Load every capture_*.csv in this folder
    2. Slide a 1-second window across the stream (like the device will)
    3. Turn each window into ~30 features (mean, std, min, max, energy...)
    4. Label each window: fall or not-fall
    5. Train a Random Forest, tested on data it never saw
    6. Print accuracy, a confusion matrix, and precision/recall
    7. Export the model to fall_model.h  ->  drop into the Arduino sketch

INSTALL
    pip install numpy pandas scikit-learn joblib

RUN
    python train_fall.py

WHY A WINDOW, NOT A SINGLE READING
    One sensor sample cannot tell you anything - 2 g could be a fall or a
    footstep. A fall is a SHAPE over about a second: quiet, then weightless,
    then a spike, then still. The features describe that shape. The device
    will compute the exact same features over its own rolling 1-second
    buffer, so what we train on is what it will see.
"""

import glob
import sys
import numpy as np
import pandas as pd

try:
    from sklearn.ensemble import RandomForestClassifier
    from sklearn.model_selection import train_test_split
    from sklearn.metrics import (confusion_matrix, classification_report,
                                 accuracy_score)
except ImportError:
    sys.exit("scikit-learn is missing.  Run:\n"
             "  pip install numpy pandas scikit-learn joblib")

# ---------------------------------------------------------------------------
#  MUST MATCH THE DEVICE
# ---------------------------------------------------------------------------
FS          = 200          # Hz, the capture and device sample rate
WIN_SEC     = 1.0          # window length in seconds
WIN         = int(FS * WIN_SEC)      # 200 samples
STEP        = WIN // 4     # 75% overlap between training windows

# Labels 7,8,9 are falls; 0-6 are everything else.
FALL_LABELS = {7, 8, 9}

# A window is "a fall" if enough of it is fall-labelled. Falls are brief, so
# we do not require the whole window - just that the fall moment is in it.
FALL_FRACTION = 0.15

# ---------------------------------------------------------------------------
#  FEATURES
#
#  Every feature here is cheap to compute on a microcontroller: sums, mins,
#  maxes, a couple of counts. No FFT, no floating-point division we cannot
#  afford. The C export computes these exact same numbers.
# ---------------------------------------------------------------------------
FEATURE_NAMES = []

def _stats(prefix, v):
    """Six summary numbers for one signal channel."""
    out = {}
    out[prefix + "_mean"] = float(np.mean(v))
    out[prefix + "_std"]  = float(np.std(v))
    out[prefix + "_min"]  = float(np.min(v))
    out[prefix + "_max"]  = float(np.max(v))
    out[prefix + "_rng"]  = float(np.max(v) - np.min(v))
    out[prefix + "_rms"]  = float(np.sqrt(np.mean(v * v)))
    return out

def extract(win):
    """win: (WIN, 7) array of ax,ay,az,gx,gy,gz,amag -> dict of features."""
    ax, ay, az = win[:, 0], win[:, 1], win[:, 2]
    gx, gy, gz = win[:, 3], win[:, 4], win[:, 5]
    amag       = win[:, 6]
    gmag = np.sqrt(gx * gx + gy * gy + gz * gz)

    f = {}
    f.update(_stats("amag", amag))
    f.update(_stats("gmag", gmag))
    f.update(_stats("ax", ax))
    f.update(_stats("ay", ay))
    f.update(_stats("az", az))

    # --- the fall-specific features: the SHAPE of the event ---
    # how far below 1 g it dipped (free-fall) and how far above it spiked
    f["amag_below05"] = float(np.mean(amag < 0.5))     # fraction weightless
    f["amag_above25"] = float(np.mean(amag > 2.5))     # fraction in impact
    f["amag_peak"]    = float(np.max(amag))
    f["amag_trough"]  = float(np.min(amag))
    # difference between the biggest spike and the deepest dip - large only
    # when both a free-fall and an impact happen in the same window
    f["amag_swing"]   = f["amag_peak"] - f["amag_trough"]
    # jerk: how fast |a| is changing, the signature of a sharp impact
    da = np.diff(amag)
    f["jerk_max"]     = float(np.max(np.abs(da))) if len(da) else 0.0
    # orientation change across the window: gravity direction start vs end
    g0 = win[:FS // 10, 0:3].mean(axis=0)
    g1 = win[-FS // 10:, 0:3].mean(axis=0)
    n0, n1 = np.linalg.norm(g0), np.linalg.norm(g1)
    if n0 > 0.1 and n1 > 0.1:
        dot = np.clip(np.dot(g0, g1) / (n0 * n1), -1, 1)
        f["tilt_deg"] = float(np.degrees(np.arccos(dot)))
    else:
        f["tilt_deg"] = 0.0
    return f


def windows_from(df):
    """Slide over one capture file, yielding (features, is_fall)."""
    cols = ["ax", "ay", "az", "gx", "gy", "gz", "amag"]
    data = df[cols].to_numpy(dtype=np.float32)
    labels = df["label"].to_numpy()
    n = len(df)

    feats, ys = [], []
    for start in range(0, n - WIN, STEP):
        w = data[start:start + WIN]
        lw = labels[start:start + WIN]
        fall_frac = np.mean(np.isin(lw, list(FALL_LABELS)))
        is_fall = 1 if fall_frac >= FALL_FRACTION else 0
        # Skip mixed windows that are neither clearly a fall nor clearly not,
        # so the model learns from clean examples.
        if 0 < fall_frac < FALL_FRACTION:
            continue
        feats.append(extract(w))
        ys.append(is_fall)
    return feats, ys


# ---------------------------------------------------------------------------
def main():
    files = sorted(glob.glob("capture_*.csv"))
    if not files:
        sys.exit("No capture_*.csv found in this folder.\n"
                 "Collect data first with capture.py.")

    print(f"Loading {len(files)} file(s): {', '.join(files)}")

    all_f, all_y = [], []
    for path in files:
        df = pd.read_csv(path)
        if "label" not in df.columns:
            print(f"  {path}: no label column, skipped")
            continue
        f, y = windows_from(df)
        all_f += f
        all_y += y
        print(f"  {path}: {len(df)} rows -> {len(y)} windows "
              f"({sum(y)} fall, {len(y) - sum(y)} normal)")

    if not all_f:
        sys.exit("No usable windows. Did you capture with labels set?")

    X = pd.DataFrame(all_f)
    y = np.array(all_y)
    global FEATURE_NAMES
    FEATURE_NAMES = list(X.columns)

    print(f"\nTotal: {len(y)} windows, {int(y.sum())} falls, "
          f"{len(y) - int(y.sum())} normal, {len(FEATURE_NAMES)} features each")

    if y.sum() < 20:
        print("\n  WARNING: fewer than 20 fall windows. The model will be "
              "weak.\n  Collect more falls (labels 7,8,9) and rerun.")

    # --- split so we TEST on data the model never trained on -------------
    Xtr, Xte, ytr, yte = train_test_split(
        X, y, test_size=0.25, random_state=42, stratify=y)

    clf = RandomForestClassifier(
        n_estimators=60, max_depth=8, min_samples_leaf=4,
        class_weight="balanced", random_state=42)
    clf.fit(Xtr, ytr)

    # --- honest evaluation ----------------------------------------------
    pred = clf.predict(Xte)
    acc = accuracy_score(yte, pred)
    cm = confusion_matrix(yte, pred)

    print("\n" + "=" * 56)
    print("  RESULTS  (on 25% of data the model never saw)")
    print("=" * 56)
    print(f"  Accuracy: {acc * 100:.1f}%\n")
    print("  Confusion matrix:")
    print("                 predicted")
    print("                 normal   fall")
    print(f"    actual normal  {cm[0,0]:5d}  {cm[0,1]:5d}")
    print(f"    actual fall    {cm[1,0]:5d}  {cm[1,1]:5d}")
    print()
    print("  ", classification_report(yte, pred,
          target_names=["normal", "fall"]).replace("\n", "\n   "))

    # The two numbers that matter for a safety device:
    tn, fp, fn, tp = cm.ravel()
    print("  For a safety device the two errors are not equal:")
    print(f"    MISSED falls (dangerous)     : {fn}")
    print(f"    FALSE alarms (annoying)      : {fp}")

    print("\n  Most important features:")
    imp = sorted(zip(FEATURE_NAMES, clf.feature_importances_),
                 key=lambda x: -x[1])
    for name, val in imp[:8]:
        print(f"    {name:<16} {val:.3f}")

    export_c(clf, "fall_model.h")
    print("\n  Exported to fall_model.h")
    print("  Next: copy it into the node sketch and wire in ml_fall.cpp")


# ---------------------------------------------------------------------------
#  C EXPORT
#
#  A Random Forest is just a set of if/else trees voting. We write each tree
#  as nested C, and a function that averages their votes. No library, no
#  floats beyond what we already use, runs in microseconds.
# ---------------------------------------------------------------------------
def export_c(clf, path):
    lines = []
    lines.append("// ==========================================================")
    lines.append("//  PHANTOM NET - fall_model.h  (AUTO-GENERATED, do not edit)")
    lines.append("//  Random Forest exported from train_fall.py")
    lines.append(f"//  {len(clf.estimators_)} trees, {len(FEATURE_NAMES)} features")
    lines.append("// ==========================================================")
    lines.append("#pragma once")
    lines.append("")
    lines.append(f"#define ML_N_FEATURES {len(FEATURE_NAMES)}")
    lines.append(f"#define ML_N_TREES    {len(clf.estimators_)}")
    lines.append("")
    lines.append("// feature order - ml_fall.cpp must fill f[] in exactly this order:")
    for i, name in enumerate(FEATURE_NAMES):
        lines.append(f"//   f[{i:2d}] = {name}")
    lines.append("")

    def tree_code(tree, idx):
        t = tree.tree_
        body = [f"static float ml_tree{idx}(const float* f) {{"]

        def recurse(node, depth):
            ind = "  " * (depth + 1)
            if t.feature[node] != -2:          # split node
                fi = t.feature[node]
                th = t.threshold[node]
                body.append(f"{ind}if (f[{fi}] <= {th:.6f}f) {{")
                recurse(t.children_left[node], depth + 1)
                body.append(f"{ind}}} else {{")
                recurse(t.children_right[node], depth + 1)
                body.append(f"{ind}}}")
            else:                              # leaf: probability of "fall"
                vals = t.value[node][0]
                p = vals[1] / (vals[0] + vals[1]) if (vals[0] + vals[1]) else 0.0
                body.append(f"{ind}return {p:.6f}f;")

        recurse(0, 0)
        body.append("}")
        return "\n".join(body)

    for i, est in enumerate(clf.estimators_):
        lines.append(tree_code(est, i))
        lines.append("")

    # the forest: average the trees' fall-probabilities
    lines.append("// Returns P(fall), 0..1. Compare against a threshold you")
    lines.append("// pick from the confusion matrix - higher = fewer false")
    lines.append("// alarms but more missed falls.")
    lines.append("static float ml_fall_probability(const float* f) {")
    lines.append("  float sum = 0.0f;")
    for i in range(len(clf.estimators_)):
        lines.append(f"  sum += ml_tree{i}(f);")
    lines.append(f"  return sum / {len(clf.estimators_)}.0f;")
    lines.append("}")
    lines.append("")

    with open(path, "w") as fh:
        fh.write("\n".join(lines))


if __name__ == "__main__":
    main()
