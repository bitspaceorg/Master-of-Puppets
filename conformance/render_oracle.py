#!/usr/bin/env python3
"""
Master of Puppets — Conformance Framework
render_oracle.py — Reference oracle renderer driver

Exports MOP's procedural scene definition and drives an external reference
renderer (e.g. Blender headless) to produce golden baseline images.

Usage:
    python3 conformance/render_oracle.py --scene=torture --output=conformance/baselines/

The oracle renderer is configured externally.  This script:
  1. Generates a scene description JSON matching the conformance stress scene
  2. Generates camera path JSON for each of the 7 paths
  3. Invokes the oracle renderer for each frame
  4. Captures output PNGs to the baselines directory

SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
import math
import os
import sys


# ---------------------------------------------------------------------------
# Camera path evaluators (mirrors camera_paths.c)
# ---------------------------------------------------------------------------

def eval_orbit(t):
    R, H = 800.0, 200.0
    w = 2.0 * math.pi / 3000.0
    return {
        "eye": [R * math.cos(t * w), H, R * math.sin(t * w)],
        "target": [0.0, 50.0, -400.0],
        "up": [0.0, 1.0, 0.0],
        "fov": 60.0,
        "near": 0.1,
        "far": 10000.0,
    }

def eval_zoom(t):
    exponent = -3.0 * t / 2000.0
    D = 1000.0 * (10.0 ** exponent)
    return {
        "eye": [0.0, 50.0, D],
        "target": [0.0, 0.0, -5.0],
        "up": [0.0, 1.0, 0.0],
        "fov": 60.0,
        "near": max(D * 0.001, 1e-6),
        "far": max(D * 100.0, 1e8),
    }

def eval_fov_sweep(t):
    return {
        "eye": [0.0, 100.0, 200.0],
        "target": [0.0, 0.0, -400.0],
        "up": [0.0, 1.0, 0.0],
        "fov": 1.0 + 178.0 * (t / 1000.0),
        "near": 0.01,
        "far": 50000.0,
    }

def eval_jitter(t):
    return {
        "eye": [
            0.0 + math.sin(t * 17.3) * 0.001,
            100.0 + math.cos(t * 23.7) * 0.001,
            200.0 + math.sin(t * 31.1) * 0.001,
        ],
        "target": [0.0, 0.0, -400.0],
        "up": [0.0, 1.0, 0.0],
        "fov": 60.0,
        "near": 0.1,
        "far": 10000.0,
    }

def eval_extreme_near(t):
    return {
        "eye": [0.0, 0.001, 0.001],
        "target": [0.0, 0.0, 0.0],
        "up": [0.0, 1.0, 0.0],
        "fov": 90.0,
        "near": 1e-6,
        "far": 1e8,
    }

def eval_transparency(t):
    w = 2.0 * math.pi / 500.0
    return {
        "eye": [200.0 + 30.0 * math.cos(t * w), 10.0,
                -400.0 + 30.0 * math.sin(t * w)],
        "target": [200.0, 0.0, -400.0],
        "up": [0.0, 1.0, 0.0],
        "fov": 60.0,
        "near": 0.1,
        "far": 5000.0,
    }

CAMERA_PATHS = {
    "ORBIT":         {"evaluator": eval_orbit,         "frames": 3000},
    "ZOOM":          {"evaluator": eval_zoom,           "frames": 2000},
    "FOV_SWEEP":     {"evaluator": eval_fov_sweep,      "frames": 1000},
    "JITTER":        {"evaluator": eval_jitter,         "frames": 2000},
    "EXTREME_NEAR":  {"evaluator": eval_extreme_near,   "frames": 500},
    "TRANSPARENCY":  {"evaluator": eval_transparency,   "frames": 500},
    # HIERARCHY_FLY requires tower positions from the C scene gen;
    # skip in oracle generation unless provided externally
}


# ---------------------------------------------------------------------------
# Scene description generation
# ---------------------------------------------------------------------------

def generate_scene_desc():
    """Generate JSON scene description matching the conformance stress scene."""
    scene = {
        "name": "mop_conformance_torture",
        "zones": {
            "A_instancing_grid": {
                "mesh": "sphere_16x32",
                "grid_size": 100,
                "spacing": 2.5,
                "instance_count": 10000,
            },
            "B_hierarchy_tower": {
                "mesh": "cylinder_16x1",
                "levels": 24,
                "angle_step_deg": 15.0,
                "scale_decay": 0.97,
            },
            "C_precision_stress": {
                "coplanar_quads": 8,
                "degenerate_tris": 64,
                "huge_ground_plane_size": 1e6,
                "micro_cube_scale": 1e-6,
                "macro_cube_scale": 1e3,
                "negative_scale_cube": True,
            },
            "D_transparency_stress": {
                "intersecting_alpha_planes": 16,
                "alpha_clipped_billboards": 16,
                "double_sided_planes": 8,
            },
            "E_lighting": {
                "directional": {
                    "direction": [-0.3, -1.0, -0.5],
                    "color": [1, 1, 1],
                    "intensity": 3.14159,
                },
                "point_lights": [
                    {"position": [-100, 50, -100], "color": [1, 0.8, 0.6],
                     "intensity": 39.478, "range": 300},
                    {"position": [100, 50, -100], "color": [0.6, 0.8, 1],
                     "intensity": 39.478, "range": 300},
                    {"position": [-100, 50, -700], "color": [1, 1, 0.8],
                     "intensity": 39.478, "range": 300},
                    {"position": [100, 50, -700], "color": [0.8, 1, 1],
                     "intensity": 39.478, "range": 300},
                ],
                "spot_lights": [
                    {"position": [0, 100, -250], "direction": [0, -1, 0],
                     "color": [1, 1, 1], "intensity": 39.478, "range": 500,
                     "inner_deg": 30, "outer_deg": 45},
                    {"position": [200, 80, -400], "direction": [0, -1, 0],
                     "color": [1, 0.9, 0.8], "intensity": 39.478,
                     "range": 400, "inner_deg": 20, "outer_deg": 35},
                ],
            },
            "F_material_stress": {
                "grid": "6x4",
                "materials": "pbr_sweep",
            },
        },
        "resolution": [1920, 1080],
    }
    return scene


def generate_camera_paths_json():
    """Generate JSON camera path data for all paths."""
    paths = {}
    for name, info in CAMERA_PATHS.items():
        evaluator = info["evaluator"]
        frames = info["frames"]
        path_data = []
        for t in range(frames):
            cam = evaluator(t)
            path_data.append({"frame": t, **cam})
        paths[name] = {"frame_count": frames, "frames": path_data}
    return paths


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="MOP Conformance Oracle Renderer Driver")
    parser.add_argument("--scene", default="torture",
                        help="Scene name (default: torture)")
    parser.add_argument("--output", default="conformance/baselines",
                        help="Output directory for golden baselines")
    parser.add_argument("--export-only", action="store_true",
                        help="Only export scene/camera JSON, skip rendering")
    parser.add_argument("--path", default=None,
                        help="Render only this camera path (e.g. ORBIT)")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    args = parser.parse_args()

    os.makedirs(args.output, exist_ok=True)

    # Export scene description
    scene_desc = generate_scene_desc()
    scene_path = os.path.join(args.output, "scene.json")
    with open(scene_path, "w") as f:
        json.dump(scene_desc, f, indent=2)
    print(f"Wrote scene description: {scene_path}")

    # Export camera paths
    paths = generate_camera_paths_json()
    paths_path = os.path.join(args.output, "camera_paths.json")
    with open(paths_path, "w") as f:
        json.dump(paths, f, indent=2)
    print(f"Wrote camera paths: {paths_path}")

    if args.export_only:
        print("Export-only mode — skipping rendering.")
        return

    # Render with external oracle
    # This is a placeholder — the actual oracle invocation depends on
    # the external renderer configuration (e.g. Blender headless).
    print("\nOracle rendering not yet configured.")
    print("To generate baselines:")
    print(f"  1. Import {scene_path} into your reference renderer")
    print(f"  2. Render each camera path frame at {args.width}x{args.height}")
    print(f"  3. Save PNGs to {args.output}/{{path_name}}/frame_NNNN.png")
    print("\nScene and camera path JSON files are ready for import.")


if __name__ == "__main__":
    main()
