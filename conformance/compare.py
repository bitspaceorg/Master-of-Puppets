#!/usr/bin/env python3
"""
Master of Puppets — Conformance Framework
compare.py — Image comparison between MOP renders and golden baselines

Computes SSIM, RMSE, PSNR between rendered frames and reference baselines.
Outputs per-frame metrics and a summary JSON report.

Usage:
    python3 conformance/compare.py \\
        --rendered=conformance/results/frames/ \\
        --reference=conformance/baselines/ORBIT/ \\
        --output=conformance/results/comparison.json

Dependencies: Pillow (pip install Pillow)
Optional:     scikit-image for SSIM (falls back to simple implementation)

SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
import math
import os
import sys

try:
    from PIL import Image
    import numpy as np
    HAS_DEPS = True
except ImportError:
    HAS_DEPS = False


# ---------------------------------------------------------------------------
# Pure-Python metrics (no numpy required, but slow)
# ---------------------------------------------------------------------------

def _rmse_pure(a_bytes, b_bytes, w, h):
    """RMSE over RGB channels, uint8 RGBA buffers."""
    total = 0.0
    n = w * h
    for i in range(n):
        idx = i * 4
        dr = a_bytes[idx] - b_bytes[idx]
        dg = a_bytes[idx + 1] - b_bytes[idx + 1]
        db = a_bytes[idx + 2] - b_bytes[idx + 2]
        total += dr * dr + dg * dg + db * db
    return math.sqrt(total / (n * 3.0))


# ---------------------------------------------------------------------------
# NumPy-accelerated metrics
# ---------------------------------------------------------------------------

def rmse_np(a, b):
    """RMSE over RGB channels using numpy arrays (H, W, C)."""
    diff = a[:, :, :3].astype(np.float64) - b[:, :, :3].astype(np.float64)
    return np.sqrt(np.mean(diff ** 2))


def psnr_np(a, b):
    """PSNR in dB."""
    r = rmse_np(a, b)
    if r == 0:
        return 100.0
    return 20.0 * math.log10(255.0 / r)


def ssim_np(a, b, win=8):
    """Simple SSIM on luminance, 8x8 sliding window (Wang et al. 2004)."""
    C1 = (0.01 * 255) ** 2
    C2 = (0.03 * 255) ** 2

    # Convert to luminance
    la = 0.299 * a[:, :, 0] + 0.587 * a[:, :, 1] + 0.114 * a[:, :, 2]
    lb = 0.299 * b[:, :, 0] + 0.587 * b[:, :, 1] + 0.114 * b[:, :, 2]
    la = la.astype(np.float64)
    lb = lb.astype(np.float64)

    h, w = la.shape
    if h < win or w < win:
        return 1.0

    ssim_sum = 0.0
    count = 0

    for y in range(0, h - win + 1, win // 2):
        for x in range(0, w - win + 1, win // 2):
            wa = la[y:y + win, x:x + win]
            wb = lb[y:y + win, x:x + win]

            mu_a = np.mean(wa)
            mu_b = np.mean(wb)
            var_a = np.var(wa)
            var_b = np.var(wb)
            cov = np.mean((wa - mu_a) * (wb - mu_b))

            num = (2 * mu_a * mu_b + C1) * (2 * cov + C2)
            den = (mu_a ** 2 + mu_b ** 2 + C1) * (var_a + var_b + C2)

            ssim_sum += num / den
            count += 1

    return ssim_sum / count if count > 0 else 1.0


# ---------------------------------------------------------------------------
# Comparison logic
# ---------------------------------------------------------------------------

def compare_images(rendered_path, reference_path):
    """Compare two images, return metrics dict."""
    img_a = Image.open(rendered_path).convert("RGBA")
    img_b = Image.open(reference_path).convert("RGBA")

    # Resize to match if needed
    if img_a.size != img_b.size:
        img_b = img_b.resize(img_a.size, Image.LANCZOS)

    a = np.array(img_a)
    b = np.array(img_b)

    metrics = {
        "rmse": float(rmse_np(a, b)),
        "psnr": float(psnr_np(a, b)),
        "ssim": float(ssim_np(a, b)),
    }

    # Verdict
    if metrics["ssim"] >= 0.92 and metrics["rmse"] <= 12.0:
        metrics["verdict"] = "PASS"
    elif metrics["ssim"] >= 0.85 and metrics["rmse"] <= 20.0:
        metrics["verdict"] = "WARN"
    else:
        metrics["verdict"] = "FAIL"

    return metrics


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="MOP Conformance Image Comparison")
    parser.add_argument("--rendered", required=True,
                        help="Directory with rendered frame PNGs")
    parser.add_argument("--reference", required=True,
                        help="Directory with reference/golden PNGs")
    parser.add_argument("--output", default="comparison.json",
                        help="Output JSON report path")
    args = parser.parse_args()

    if not HAS_DEPS:
        print("ERROR: This script requires Pillow and numpy.")
        print("  pip install Pillow numpy")
        sys.exit(1)

    if not os.path.isdir(args.rendered):
        print(f"ERROR: Rendered directory not found: {args.rendered}")
        sys.exit(1)
    if not os.path.isdir(args.reference):
        print(f"ERROR: Reference directory not found: {args.reference}")
        sys.exit(1)

    # Find matching frame pairs
    rendered_files = sorted(
        f for f in os.listdir(args.rendered)
        if f.endswith(".png")
    )
    reference_files = sorted(
        f for f in os.listdir(args.reference)
        if f.endswith(".png")
    )

    # Match by filename
    ref_set = set(reference_files)
    pairs = [(f, f) for f in rendered_files if f in ref_set]

    if not pairs:
        print("WARNING: No matching frame pairs found.")
        print(f"  Rendered: {len(rendered_files)} files")
        print(f"  Reference: {len(reference_files)} files")
        sys.exit(0)

    print(f"Comparing {len(pairs)} frame pairs...")

    results = []
    pass_count = 0
    warn_count = 0
    fail_count = 0
    ssim_values = []
    rmse_values = []

    for rendered_name, ref_name in pairs:
        rendered_path = os.path.join(args.rendered, rendered_name)
        reference_path = os.path.join(args.reference, ref_name)

        metrics = compare_images(rendered_path, reference_path)
        metrics["frame"] = rendered_name

        results.append(metrics)
        ssim_values.append(metrics["ssim"])
        rmse_values.append(metrics["rmse"])

        if metrics["verdict"] == "PASS":
            pass_count += 1
        elif metrics["verdict"] == "WARN":
            warn_count += 1
        else:
            fail_count += 1

    # Summary
    summary = {
        "total_frames": len(pairs),
        "pass": pass_count,
        "warn": warn_count,
        "fail": fail_count,
        "mean_ssim": sum(ssim_values) / len(ssim_values),
        "min_ssim": min(ssim_values),
        "mean_rmse": sum(rmse_values) / len(rmse_values),
        "max_rmse": max(rmse_values),
        "overall": "PASS" if fail_count == 0 and warn_count == 0
                   else "WARN" if fail_count == 0
                   else "FAIL",
    }

    report = {"summary": summary, "frames": results}

    with open(args.output, "w") as f:
        json.dump(report, f, indent=2)

    print(f"\nResults written to: {args.output}")
    print(f"  Overall: {summary['overall']}")
    print(f"  PASS: {pass_count}  WARN: {warn_count}  FAIL: {fail_count}")
    print(f"  Mean SSIM: {summary['mean_ssim']:.4f}")
    print(f"  Mean RMSE: {summary['mean_rmse']:.2f}")


if __name__ == "__main__":
    main()
