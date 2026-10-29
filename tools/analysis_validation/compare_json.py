#!/usr/bin/env python3
"""Compare two JSON outputs of a ported analysis script key by key: numeric
lists to a relative tolerance, everything else exactly (paths excepted).
Exits nonzero on a mismatch.

--scale-by-key measures each value's difference against the largest
magnitude any value under the same key reaches in the reference output,
instead of against the value itself. That is the right yardstick for flux
sign bins and net fluxes on real data: a sign bin holding only cells whose
normal velocity is nearly zero, or a net that is the difference of two
nearly equal bins, is ill-conditioned, and its relative error says nothing
about the integral's accuracy."""

import argparse
import json
import sys

import numpy as np

IGNORED = {"plotfile"}


def key_scales(value, scales, key=None):
    if isinstance(value, dict):
        for name, item in value.items():
            key_scales(item, scales, name)
    elif isinstance(value, list):
        for item in value:
            key_scales(item, scales, key)
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        scales[key] = max(scales.get(key, 0.0), abs(float(value)))
    return scales


SCALES = None


def compare(left, right, rtol, path=""):
    problems = []
    if isinstance(left, dict) and isinstance(right, dict):
        if sorted(left) != sorted(right):
            problems.append(f"{path}: keys differ: {set(left) ^ set(right)}")
        for key in sorted(set(left) & set(right)):
            if key not in IGNORED:
                problems += compare(left[key], right[key], rtol, f"{path}.{key}")
        return problems
    if (isinstance(left, list) and isinstance(right, list)
            and any(isinstance(v, (dict, list)) for v in left + right)
            and not all(isinstance(v, list) and all(isinstance(w, (int, float)) or w is None for w in v)
                        for v in left + right)):
        if len(left) != len(right):
            return [f"{path}: lengths {len(left)} != {len(right)}"]
        for index, (a, b) in enumerate(zip(left, right)):
            problems += compare(a, b, rtol, f"{path}[{index}]")
        return problems
    if left is None or right is None:
        return [] if left is right else [f"{path}: {left!r} != {right!r}"]
    try:
        a = np.asarray(left, dtype=float)
        b = np.asarray(right, dtype=float)
    except (TypeError, ValueError):
        return [] if left == right else [f"{path}: {left!r} != {right!r}"]
    if a.shape != b.shape:
        return [f"{path}: shapes {a.shape} != {b.shape}"]
    scale = np.maximum(np.abs(a), np.abs(b))
    if SCALES is not None:
        key = path.rsplit(".", 1)[-1].split("[", 1)[0]
        scale = np.maximum(scale, SCALES.get(key, 0.0))
    bad = np.abs(a - b) > rtol * np.where(scale > 0, scale, 1.0)
    if np.any(bad):
        worst = np.max(np.abs(a - b)[bad] / np.maximum(scale[bad], 1e-300))
        problems.append(f"{path}: {bad.sum()} values differ, worst rel {worst:.3g}")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("left")
    parser.add_argument("right")
    parser.add_argument("--rtol", type=float, default=1e-12)
    parser.add_argument("--scale-by-key", action="store_true")
    args = parser.parse_args()
    global SCALES
    with open(args.left) as a, open(args.right) as b:
        left, right = json.load(a), json.load(b)
    if args.scale_by_key:
        SCALES = key_scales(right, {})
    problems = compare(left, right, args.rtol)
    for problem in problems:
        print(problem)
    print("match" if not problems else f"{len(problems)} mismatches")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
