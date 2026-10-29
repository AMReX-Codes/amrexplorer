#!/usr/bin/env python3
"""Plot surface density versus cylindrical radius R from 3-D plotfiles.

A port of incite-2026's ``postprocessing/plot_surface_density_profile.py``
from yt to amrexplorer's exact projection (docs/analysis-scripting-interface.md).
Pass a single plotfile or a directory of plotfiles. Volume densities are
integrated along --axis (default: z) through the whole domain and averaged
in circular annuli of the image about --center-kpc (default: domain center).
--width-kpc limits the sampled plane. Use --sfr for deposition fields already
normalized to a mass formation rate (g cm^-3 s^-1). Outputs, a PNG and a CSV
per plotfile, are the yt script's.

The one deliberate difference: yt samples its projection at pixel centers
(a fixed-resolution buffer), while this averages each pixel's column
exactly. The two agree wherever a pixel lies within one leaf cell's column,
which is every pixel when pixels nest in the finest cells. Only the 3-D
volume mode is ported; the yt script's 2-D "projection plotfile" mode is not.
The plotfile is assumed to be cgs, as Quokka writes it.

Validates: analysis totals.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Optional, Sequence, Tuple

import numpy as np

import amrexplorer

AXIS_NAMES = ["x", "y", "z"]
PROFILE_DISPLAY_UNIT = "Msun/pc**2"
SFR_DISPLAY_UNIT = "Msun/yr/kpc**2"
SFR_FIELD = "particle.StochasticStellarPop.birth_mass_density"

# unyt's values, as the yt script converts with them.
MSUN_G = 1.98841586e33
PC_CM = 3.0856775809623245e18
KPC_CM = 3.085677580962325e21
YR_S = 31557600.0
MYR_S = 3.15576e13


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input", type=Path,
                        help="Directory of plotfiles or a single 3D plotfile")
    parser.add_argument("--axis", choices=AXIS_NAMES, default=None,
                        help="Projection axis (default: z)")
    parser.add_argument("--sfr", action="store_true", help=(
        "Profile already time-normalized SFR deposition in Msun/yr/kpc**2 "
        f"(default field: {SFR_FIELD}); no further age-window division"))
    parser.add_argument("--field", default=None,
                        help="Density field to profile (default: auto-detect)")
    parser.add_argument("--center-kpc", type=float, nargs=2, metavar=("C0", "C1"),
                        help="Galactic center in the two in-plane coordinates, "
                             "in kpc (default: dataset domain center)")
    parser.add_argument("--width-kpc", type=float, default=None,
                        help="Square region width in kpc centered on the "
                             "galactic center (default: full domain extent in "
                             "the projection plane)")
    parser.add_argument("--resolution", type=int, default=1024,
                        help="Sampling resolution per in-plane axis (default: 1024)")
    parser.add_argument("--n-bins", type=int, default=128,
                        help="Number of radial bins (default: 128)")
    parser.add_argument("--r-max-kpc", type=float, default=15.0,
                        help="Maximum galactocentric radius in kpc (default: 15)")
    parser.add_argument("--output-dir", type=Path, default=None,
                        help="Directory for output plot and CSV files "
                             "(default: <input>/pngs)")
    parser.add_argument("--output-suffix", default="surface_density_profile",
                        help="Suffix for output filenames "
                             "(default: surface_density_profile)")
    parser.add_argument("--no-csv", action="store_true",
                        help="Disable writing the radial profile CSV")
    parser.add_argument("--overwrite", action="store_true",
                        help="Overwrite existing PNG and CSV outputs")
    return parser.parse_args()


def _find_plotfiles(input_path: Path) -> list[Path]:
    if (input_path / "Header").is_file():
        return [input_path]
    if input_path.is_dir():
        return sorted(entry for entry in input_path.iterdir()
                      if entry.is_dir() and (entry / "Header").is_file())
    return []


def _projection_plane_indices(normal_axis: int) -> Tuple[int, int]:
    return {0: (1, 2), 1: (0, 2), 2: (0, 1)}[normal_axis]


def _select_field(ds, requested: Optional[str]) -> str:
    if requested is not None:
        if requested in ds.fields:
            return requested
        raise RuntimeError(f"Requested field '{requested}' was not found.")
    for name in ("gasDensity", "density", "Density", "rho"):
        if name in ds.fields:
            return name
    raise RuntimeError("Could not auto-detect a density field. "
                       "Use --field to specify one explicitly.")


def _sampling_region(ds, plane_axes, center_kpc, width_kpc):
    lower = np.asarray(ds.geometry["domain_lower"], dtype=float)
    upper = np.asarray(ds.geometry["domain_upper"], dtype=float)
    if width_kpc is None:
        return lower.copy(), upper.copy()
    half_width = 0.5 * width_kpc * KPC_CM
    left, right = lower.copy(), upper.copy()
    for axis, center in zip(plane_axes, center_kpc):
        center_cm = center * KPC_CM
        left[axis] = max(center_cm - half_width, lower[axis] + 1.0e-12)
        right[axis] = min(center_cm + half_width, upper[axis] - 1.0e-12)
    return left, right


def sample_projected_density(ds, field, axis, center_kpc, width_kpc,
                             resolution, sfr=False):
    """Pixel radii (kpc) and the exact pixel-averaged column integral."""
    normal = AXIS_NAMES.index(axis)
    plane_axes = _projection_plane_indices(normal)
    left, right = _sampling_region(ds, plane_axes, center_kpc, width_kpc)
    u, v = plane_axes
    projection = ds.project(field, normal,
                            rect=(left[u], left[v], right[u], right[v]),
                            bounds=(left[normal], right[normal]),
                            resolution=(resolution, resolution))
    if sfr:
        sigma = projection.image * (YR_S * KPC_CM**2 / MSUN_G)
    else:
        sigma = projection.image * (PC_CM**2 / MSUN_G)
    coords = {}
    for index in plane_axes:
        lo = left[index] / KPC_CM
        step = (right[index] - left[index]) / KPC_CM / resolution
        coords[index] = lo + (np.arange(resolution) + 0.5) * step
    coord_h, coord_v = np.meshgrid(coords[u], coords[v])
    centers = dict(zip(plane_axes, center_kpc))
    radius = np.hypot(coord_h - centers[u], coord_v - centers[v])
    return radius.ravel(), np.asarray(sigma).ravel()


def compute_radial_profile(radius_kpc, surface_density, n_bins, r_max_kpc):
    valid = (np.isfinite(radius_kpc) & np.isfinite(surface_density)
             & (radius_kpc >= 0.0) & (radius_kpc <= r_max_kpc))
    if not np.any(valid):
        raise RuntimeError("No finite samples were found inside the requested radius.")
    edges = np.linspace(0.0, r_max_kpc, n_bins + 1)
    radii = radius_kpc[valid]
    sigma = surface_density[valid]
    bin_index = np.digitize(radii, edges, right=False) - 1
    in_range = (bin_index >= 0) & (bin_index < n_bins)
    if not np.any(in_range):
        raise RuntimeError("No samples fell into the radial bins.")
    bin_index = bin_index[in_range]
    sigma = sigma[in_range]
    counts = np.bincount(bin_index, minlength=n_bins)
    sums = np.bincount(bin_index, weights=sigma, minlength=n_bins)
    means = np.full(n_bins, np.nan, dtype=float)
    nonzero = counts > 0
    means[nonzero] = sums[nonzero] / counts[nonzero]
    centers = 0.5 * (edges[:-1] + edges[1:])
    return centers, means, counts


def _plot_profile(output_path, radius_kpc, sigma_profile, field_label, title,
                  simulation_time, initial_profile=None,
                  display_unit=PROFILE_DISPLAY_UNIT):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    valid = np.isfinite(sigma_profile) & (sigma_profile > 0.0)
    if not np.any(valid):
        raise RuntimeError("The radial profile has no positive bins to plot.")
    fig, ax = plt.subplots(figsize=(6.5, 4.8), constrained_layout=True)
    if initial_profile is not None:
        initial_radius, initial_sigma = initial_profile
        initial_valid = np.isfinite(initial_sigma) & (initial_sigma > 0.0)
        if np.any(initial_valid):
            ax.plot(initial_radius[initial_valid], initial_sigma[initial_valid],
                    linestyle="--", linewidth=1.8, label="t = 0")
    ax.plot(radius_kpc[valid], sigma_profile[valid], linewidth=2.0,
            label=simulation_time)
    ax.set_xlim(0.0, float(np.nanmax(radius_kpc)))
    ax.set_yscale("log")
    if display_unit == PROFILE_DISPLAY_UNIT:
        ax.set_ylim(10**-1, 10**3)
    ax.set_xlabel(r"$R\ [{\rm kpc}]$")
    ax.set_ylabel(f"{field_label} [{display_unit}]")
    ax.set_title(title)
    ax.grid(alpha=0.25)
    if initial_profile is not None:
        ax.legend()
    fig.savefig(output_path, dpi=180, metadata={"simulation_time": simulation_time})
    plt.close(fig)


def _write_csv(output_path, radius_kpc, sigma_profile, counts,
               display_unit=PROFILE_DISPLAY_UNIT):
    data = np.column_stack([radius_kpc, sigma_profile, counts])
    value_column = ("sfr_surface_density_msun_yr_kpc2"
                    if display_unit == SFR_DISPLAY_UNIT
                    else "surface_density_msun_pc2")
    header = f"radius_kpc,{value_column},n_samples"
    np.savetxt(output_path, data, delimiter=",", header=header, comments="")


def compute_plotfile_profile(args, plotfile: Path) -> dict:
    sfr = getattr(args, "sfr", False)
    display_unit = SFR_DISPLAY_UNIT if sfr else PROFILE_DISPLAY_UNIT
    with amrexplorer.open_plotfile(plotfile) as ds:
        if ds.dimension != 3:
            raise RuntimeError("Only 3-D volume plotfiles are supported.")
        field = _select_field(ds, args.field or (SFR_FIELD if sfr else None))
        axis = args.axis or "z"
        plane_axes = _projection_plane_indices(AXIS_NAMES.index(axis))
        if args.center_kpc is not None:
            center_kpc = (float(args.center_kpc[0]), float(args.center_kpc[1]))
        else:
            lower = ds.geometry["domain_lower"]
            upper = ds.geometry["domain_upper"]
            center_kpc = tuple((0.5 * lower[i] + 0.5 * upper[i]) / KPC_CM
                               for i in plane_axes)
        radius, sigma = sample_projected_density(
            ds, field, axis, center_kpc, args.width_kpc, args.resolution, sfr)
        time = ds.time
    profile_radius, sigma_profile, counts = compute_radial_profile(
        radius, sigma, args.n_bins, args.r_max_kpc)
    return {"plotfile": plotfile, "display_unit": display_unit, "field": field,
            "axis": axis, "profile_radius_kpc": profile_radius,
            "sigma_profile": sigma_profile, "counts": counts,
            "simulation_time": f"{time} code_time",
            "simulation_time_value": time / MYR_S}


def _write_outputs(args, data, initial_profile=None):
    plotfile = data["plotfile"]
    output_base = args.output_dir / f"{plotfile.name}_{args.output_suffix}"
    png_path = output_base.with_suffix(".png")
    display_unit = data["display_unit"]
    field_label = (r"$\Sigma_{\rm SFR}$" if display_unit == SFR_DISPLAY_UNIT
                   else str(data["field"]))
    wrote_png = False
    if png_path.exists() and not args.overwrite:
        print(f"Skipping existing PNG {png_path}")
    else:
        _plot_profile(png_path, data["profile_radius_kpc"], data["sigma_profile"],
                      field_label, plotfile.name, data["simulation_time"],
                      initial_profile, display_unit)
        wrote_png = True
    if not args.no_csv:
        csv_path = output_base.with_suffix(".csv")
        if csv_path.exists() and not args.overwrite:
            print(f"Skipping existing CSV {csv_path}")
        else:
            _write_csv(csv_path, data["profile_radius_kpc"], data["sigma_profile"],
                       data["counts"], display_unit)
    print(f"{'Wrote' if wrote_png else 'Preserved'} {png_path} "
          f"for field {data['field']} on axis {data['axis']}")


def main() -> None:
    args = _parse_args()
    if args.sfr and args.output_suffix == "surface_density_profile":
        args.output_suffix = "sfr_surface_density_profile"
    if args.resolution <= 0:
        raise ValueError("--resolution must be positive")
    if args.n_bins <= 0:
        raise ValueError("--n-bins must be positive")
    if args.width_kpc is not None and args.width_kpc <= 0.0:
        raise ValueError("--width-kpc must be positive when provided")
    if args.r_max_kpc is not None and args.r_max_kpc <= 0.0:
        raise ValueError("--r-max-kpc must be positive when provided")
    plotfiles = _find_plotfiles(args.input)
    if not plotfiles:
        raise RuntimeError(f"No plotfiles found under {args.input}")
    if args.output_dir is None:
        args.output_dir = args.input / "pngs"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    profiles = [compute_plotfile_profile(args, plotfile) for plotfile in plotfiles]
    initial = min(profiles, key=lambda item: item["simulation_time_value"])
    for item in profiles:
        _write_outputs(args, item, None if item is initial else
                       (initial["profile_radius_kpc"], initial["sigma_profile"]))


if __name__ == "__main__":
    main()
