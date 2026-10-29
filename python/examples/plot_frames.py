#!/usr/bin/env python3
"""Draw slice and projection frames for a plotfile sequence, headlessly.

Draws, with Matplotlib, a zoomed slice or projection about a center, with
fixed color limits, a scale bar, a timestamp and, when the plotfile has them,
magnetic-field streamlines. Each frame is skipped if its PNG exists.

    plot_frames.py plt* --kind slice --axis x --field gasDensity --zoom 40 --zlim 1e-28 1e-23
    plot_frames.py plt* --kind projection --axis z --zoom 40 --zlim 1e-4 1

Slices are exact cell averages over each pixel (Dataset.slice_cell_average)
and projections exact column integrals (Dataset.project), both in the
plotfile's native units; --sampled draws the point-sampled slice yt's slice
plot resembles. Streamlines follow the in-plane field sampled at pixel
centers (Dataset.slice).

Validates: visual equivalence.
"""

from __future__ import annotations

import argparse
import sys

import numpy as np

import amrexplorer
from amrexplorer import sequence

AXES = ("x", "y", "z")
SECONDS_PER_MYR = 3.15576e13
CM_PER_KPC = 3.085677580962325e21


def _scale_bar(ax, extent_kpc):
    width = extent_kpc[1] - extent_kpc[0]
    length = 10 ** np.floor(np.log10(width / 4))
    for factor in (5, 2, 1):
        if factor * length <= width / 4:
            length *= factor
            break
    x0 = extent_kpc[0] + 0.06 * width
    y0 = extent_kpc[2] + 0.06 * (extent_kpc[3] - extent_kpc[2])
    ax.plot([x0, x0 + length], [y0, y0], color="white", linewidth=3)
    label = f"{length:g} kpc" if length >= 1 else f"{length * 1e3:g} pc"
    ax.text(x0 + 0.5 * length, y0, label, color="white", ha="center",
            va="bottom", fontsize=9)


def draw(ds, args, output):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.colors import LogNorm

    axis = AXES.index(args.axis)
    u, v = {0: (1, 2), 1: (0, 2), 2: (0, 1)}[axis]
    lower = np.asarray(ds.geometry["domain_lower"])
    upper = np.asarray(ds.geometry["domain_upper"])
    center = (0.5 * lower + 0.5 * upper if args.center is None
              else np.asarray(args.center, dtype=float))
    half = 0.5 * (upper - lower) / args.zoom
    rect = (center[u] - half[u], center[v] - half[v],
            center[u] + half[u], center[v] + half[v])
    resolution = (args.resolution, args.resolution)
    if args.kind == "projection":
        result = ds.project(args.field, axis, rect=rect, resolution=resolution)
        image = result.image
        label = f"{args.field} column [native units x length]"
    elif args.sampled:
        result = ds.slice(args.field, axis, center[axis], rect=rect,
                          resolution=resolution)
        image = result.masked()
        label = f"{args.field} [native units]"
    else:
        result = ds.slice_cell_average(args.field, axis, center[axis], rect=rect,
                                       resolution=resolution)
        image = result.image
        label = f"{args.field} [native units]"

    extent = [value / CM_PER_KPC for value in (rect[0], rect[2], rect[1], rect[3])]
    fig, ax = plt.subplots(figsize=(7, 6))
    norm = LogNorm(*args.zlim) if args.zlim else LogNorm()
    with np.errstate(invalid="ignore", divide="ignore"):
        im = ax.imshow(np.ma.masked_invalid(np.ma.masked_less_equal(image, 0)),
                       origin="lower", extent=extent, cmap=args.cmap, norm=norm)
    fig.colorbar(im, ax=ax, label=label)

    names = [f"{AXES[u]}-BField", f"{AXES[v]}-BField"]
    if not args.no_streamlines and all(n in ds.fields for n in names):
        coarse = (args.streamline_resolution,) * 2
        bu, bv = (ds.slice(n, axis, center[axis], rect=rect, resolution=coarse)
                  for n in names)
        ax.streamplot(bu.u / CM_PER_KPC, bu.v / CM_PER_KPC,
                      np.where(bu.valid, bu.values, 0.0),
                      np.where(bv.valid, bv.values, 0.0),
                      color="white", linewidth=0.8, density=1.2)
        ax.set_xlim(extent[0], extent[1])
        ax.set_ylim(extent[2], extent[3])

    _scale_bar(ax, extent)
    ax.text(0.03, 0.96, f"t = {ds.time / SECONDS_PER_MYR:.2f} Myr",
            transform=ax.transAxes, color="white", va="top")
    ax.set_xlabel(f"{AXES[u]} [kpc]")
    ax.set_ylabel(f"{AXES[v]} [kpc]")
    kind = "Projection" if args.kind == "projection" else "Slice"
    ax.set_title(f"{kind} {args.axis} of {args.field}")
    fig.tight_layout()
    fig.savefig(output, dpi=args.dpi)
    plt.close(fig)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("plotfiles", nargs="+",
                   help="plotfiles, directories of them, or glob patterns")
    p.add_argument("--kind", choices=("slice", "projection"), default="slice")
    p.add_argument("--axis", choices=AXES, default="x")
    p.add_argument("--field", default="gasDensity")
    p.add_argument("--zoom", type=float, default=1.0)
    p.add_argument("--center", type=float, nargs=3, metavar=("X", "Y", "Z"),
                   help="native coordinates (default: domain center)")
    p.add_argument("--zlim", type=float, nargs=2, metavar=("MIN", "MAX"))
    p.add_argument("--resolution", type=int, default=800)
    p.add_argument("--streamline-resolution", type=int, default=48)
    p.add_argument("--no-streamlines", action="store_true")
    p.add_argument("--sampled", action="store_true",
                   help="point-sample slices instead of averaging cells")
    p.add_argument("--cmap", default="viridis")
    p.add_argument("--dpi", type=int, default=150)
    p.add_argument("--output-dir")
    p.add_argument("--overwrite", action="store_true")
    args = p.parse_args(argv)

    kind = "projection" if args.kind == "projection" else "Slice"
    suffix = f"_{kind}_{args.axis}_{args.field}.png"
    paths = sequence.find_plotfiles(args.plotfiles, prefix=None)
    if not paths:
        print("no plotfiles found", file=sys.stderr)
        return 1
    for path in paths:
        output = sequence.output_path(path, suffix, args.output_dir)
        if not sequence.needs_output(output, args.overwrite):
            continue
        with amrexplorer.open_plotfile(path) as ds:
            draw(ds, args, output)
        print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
