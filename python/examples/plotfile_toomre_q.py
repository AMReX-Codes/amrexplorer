#!/usr/bin/env python3
"""Compute and plot radially binned gas Toomre-Q profiles from Quokka plotfiles.

A port of Kangaroo's ``scripts/plotfile_toomre_q.py`` to amrexplorer
(docs/analysis-scripting-interface.md). Arguments, the CSV and both plots are
Kangaroo's, and so is everything downstream of the annular moments
(derive_toomre_profiles and the output code are copied unchanged). The
moments are accumulated over exact leaf cells: each cell is binned by the
cylindrical radius of its center about --center-kpc, weighted by the part of
it inside the z bounds. The radial gravity needs the potential's gradient,
which comes from tiles with one ghost cell (Dataset.plan_tiles) by central
differences, ghost cells at coarse-fine boundaries interpolated from the
coarser level. Kangaroo differentiates with its own sample-based stencil, so
the radial gravity -- and the circular speed, kappa and Q derived from it --
agree with Kangaroo only to the accuracy of the two discretizations, exactly
only where both are exact (a potential quadratic in x and y on a uniform
level). The plotfile is assumed to be cgs, as Quokka writes it.

Validates: analysis totals.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path
from typing import Iterable

import matplotlib
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

import amrexplorer  # noqa: E402

G_CGS = 6.67430e-8
PC_CM = 3.0856775814913673e18
KPC_CM = 1.0e3 * PC_CM
MSUN_G = 1.98847e33
MYR_S = 1.0e6 * 365.25 * 24.0 * 3600.0
NUM_MOMENTS = 7

FIELD_CANDIDATES = {
    "density": ("gasDensity", "density", "rho", "Density"),
    "momx": ("x-GasMomentum", "xmom", "momx", "x1Momentum", "momentum_x"),
    "momy": ("y-GasMomentum", "ymom", "momy", "x2Momentum", "momentum_y"),
    "internal_energy": (
        "gasInternalEnergy",
        "internal_energy",
        "eint",
        "Eint",
    ),
    "bx": ("x-BField", "Bx", "bx", "magnetic_x", "B_x"),
    "by": ("y-BField", "By", "by", "magnetic_y", "B_y"),
    "bz": ("z-BField", "Bz", "bz", "magnetic_z", "B_z"),
    "potential": ("gpot", "potential", "gravitational_potential", "phi"),
}

Q_COLUMNS = (
    "q_thermal_magnetic",
    "q_thermal_turbulent",
    "q_thermal_turbulent_magnetic",
)


def _pick_field(role: str, explicit: str | None, available: Iterable[str]) -> str:
    names = list(available)
    name_set = set(names)
    if explicit:
        if explicit not in name_set:
            raise RuntimeError(
                f"Field {explicit!r} for {role!r} is not listed in plotfile metadata. "
                f"Use --list-fields; available fields include: {', '.join(names[:40])}"
            )
        return explicit
    for candidate in FIELD_CANDIDATES[role]:
        if candidate in name_set:
            return candidate
    raise RuntimeError(
        f"Could not infer field for {role!r}. Pass --{role.replace('_', '-')}; "
        f"available fields include: {', '.join(names[:40])}"
    )


def _resolve_fields(ds, args: argparse.Namespace) -> dict[str, str]:
    available = ds.fields
    resolved: dict[str, str] = {}
    for role in FIELD_CANDIDATES:
        explicit = getattr(args, role)
        resolved[role] = _pick_field(role, explicit.strip() if explicit else None,
                                     available)
    return resolved


def _domain_center(ds) -> tuple[float, float, float]:
    if ds.dimension != 3:
        raise RuntimeError("Plotfile metadata does not provide three-dimensional domain bounds")
    lower, upper = ds.geometry["domain_lower"], ds.geometry["domain_upper"]
    return tuple(0.5 * (float(lo) + float(hi)) for lo, hi in zip(lower, upper))


def annular_moments(ds, fields: dict[str, str], radial_edges: np.ndarray,
                    z_bounds: tuple[float, float], center, progress=False) -> np.ndarray:
    """The (bins, 7) annular moments Kangaroo's toomre_profile_accumulate
    reduces: mass, internal energy, B^2 volume, radial momentum, its second
    moment, the radial-gravity moment and the sampled volume."""
    bins = len(radial_edges) - 1
    rmin, rmax = float(radial_edges[0]), float(radial_edges[-1])
    zmin, zmax = z_bounds
    dx0 = ds.levels[0]["cell_size"]
    # Cells binned by center within rmax in x and y, and every cell that
    # overlaps the z bounds: a center within half a level-0 cell of them.
    region = ([center[0] - rmax, center[1] - rmax, zmin - 0.5 * dx0[2]],
              [np.nextafter(center[0] + rmax, np.inf),
               np.nextafter(center[1] + rmax, np.inf),
               np.nextafter(zmax + 0.5 * dx0[2], np.inf)])
    names = [fields[role] for role in ("density", "momx", "momy", "internal_energy",
                                       "bx", "by", "bz", "potential")]
    names = list(dict.fromkeys(names))
    out = np.zeros((bins, NUM_MOMENTS), dtype=np.float64)
    plan = ds.plan_tiles(names, ghost=1, region=region, dtype=np.float64)
    for count, item in enumerate(plan, start=1):
        tile = ds.read_tile(item)
        phi = tile[fields["potential"]]
        grad_x = np.full(phi.shape, np.nan)
        grad_y = np.full(phi.shape, np.nan)
        grad_x[1:-1] = (phi[2:] - phi[:-2]) / (2.0 * tile.dx)
        grad_y[:, 1:-1] = (phi[:, 2:] - phi[:, :-2]) / (2.0 * tile.dy)
        mask = tile.leaf_mask
        origin = tile._native.index_origin
        # Cell faces and centers as Kangaroo computes them.
        faces = [origin[a] + np.arange(tile.grown_box[0][a], tile.grown_box[1][a] + 2)
                 * tile.cell_size[a] for a in range(3)]
        x = 0.5 * (faces[0][:-1] + faces[0][1:]) - center[0]
        y = 0.5 * (faces[1][:-1] + faces[1][1:]) - center[1]
        z0, z1 = faces[2][:-1], faces[2][1:]
        rx, ry = np.meshgrid(x, y, indexing="ij")
        radius = np.sqrt(rx * rx + ry * ry)[:, :, None] * np.ones(phi.shape)
        overlap = np.maximum(0.0, np.minimum(z1, zmax) - np.maximum(z0, zmin))
        overlap = np.broadcast_to(overlap[None, None, :], phi.shape)
        rx = np.broadcast_to(rx[:, :, None], phi.shape)
        ry = np.broadcast_to(ry[:, :, None], phi.shape)
        keep = mask & (radius >= rmin) & (radius <= rmax) & (overlap > 0.0)
        rho = tile[fields["density"]][keep]
        momx = tile[fields["momx"]][keep]
        momy = tile[fields["momy"]][keep]
        eint = tile[fields["internal_energy"]][keep]
        bx = tile[fields["bx"]][keep]
        by = tile[fields["by"]][keep]
        bz = tile[fields["bz"]][keep]
        gx, gy = grad_x[keep], grad_y[keep]
        r, ox, oy = radius[keep], rx[keep], ry[keep]
        good = (np.isfinite(rho) & (rho > 0.0) & np.isfinite(momx) & np.isfinite(momy)
                & np.isfinite(eint) & np.isfinite(bx) & np.isfinite(by)
                & np.isfinite(bz) & np.isfinite(gx) & np.isfinite(gy))
        volume = tile.dx * tile.dy * overlap[keep]
        mass = rho * volume
        at_center = r == 0.0
        with np.errstate(divide="ignore", invalid="ignore"):
            vr = np.where(at_center, 0.0, (ox * momx + oy * momy) / (r * rho))
            gr = np.where(at_center, 0.0, (ox * gx + oy * gy) / r)
        good &= np.isfinite(vr) & np.isfinite(gr)
        upper = np.searchsorted(radial_edges, r, side="right")
        bin_index = np.where(r == rmax, bins - 1, upper - 1)
        good &= (bin_index >= 0) & (bin_index < bins)
        columns = (mass, eint * volume, (bx * bx + by * by + bz * bz) * volume,
                   mass * vr, mass * vr * vr, mass * gr, volume)
        for component, values in enumerate(columns):
            np.add.at(out[:, component], bin_index[good], values[good])
        if progress:
            print(f"\rtiles {count}/{len(plan)}", end="" if count < len(plan) else "\n",
                  file=sys.stderr, flush=True)
    return out


def _find_plotfiles(input_path: Path) -> list[Path]:
    path = input_path.expanduser().resolve()
    if (path / "Header").is_file():
        return [path]
    if not path.is_dir():
        raise RuntimeError(f"Input is not an AMReX plotfile or directory: {path}")
    plotfiles = sorted(
        child
        for child in path.iterdir()
        if child.is_dir() and child.name.startswith("plt") and (child / "Header").is_file()
    )
    if not plotfiles:
        raise RuntimeError(f"No immediate plt*/Header plotfiles found under {path}")
    return plotfiles


def _contiguous_finite_gradient(values: np.ndarray, coordinates: np.ndarray) -> np.ndarray:
    """Differentiate finite runs without bridging gaps in an invalid profile."""

    values = np.asarray(values, dtype=np.float64)
    coordinates = np.asarray(coordinates, dtype=np.float64)
    if values.shape != coordinates.shape:
        raise ValueError("values and coordinates must have the same shape")
    derivative = np.full_like(values, np.nan)
    finite = np.isfinite(values) & np.isfinite(coordinates)
    start = 0
    while start < len(values):
        while start < len(values) and not finite[start]:
            start += 1
        stop = start
        while stop < len(values) and finite[stop]:
            stop += 1
        count = stop - start
        if count >= 2:
            derivative[start:stop] = np.gradient(
                values[start:stop],
                coordinates[start:stop],
                edge_order=2 if count >= 3 else 1,
            )
        start = stop
    return derivative


def _nonnegative_variance(second_moment: np.ndarray, mean: np.ndarray) -> np.ndarray:
    variance = second_moment - mean * mean
    scale = np.maximum(np.abs(second_moment), mean * mean)
    roundoff = 64.0 * np.finfo(np.float64).eps * np.maximum(scale, 1.0)
    variance[(variance < 0.0) & (variance >= -roundoff)] = 0.0
    variance[variance < 0.0] = np.nan
    return variance


def derive_toomre_profiles(
    radial_edges: np.ndarray,
    moments: np.ndarray,
    *,
    gamma: float = 5.0 / 3.0,
    gravitational_constant: float = G_CGS,
) -> dict[str, np.ndarray]:
    """Convert reduced annular moments into physical profiles and three Q variants."""

    edges = np.asarray(radial_edges, dtype=np.float64)
    values = np.asarray(moments, dtype=np.float64)
    if edges.ndim != 1 or len(edges) < 4 or np.any(~np.isfinite(edges)):
        raise ValueError("radial_edges must contain at least four finite edges")
    if np.any(np.diff(edges) <= 0.0):
        raise ValueError("radial_edges must be strictly increasing")
    if values.shape != (len(edges) - 1, NUM_MOMENTS):
        raise ValueError("moments must have shape (len(radial_edges) - 1, 7)")
    if not math.isfinite(gamma) or gamma <= 1.0:
        raise ValueError("gamma must be finite and greater than 1")
    if not math.isfinite(gravitational_constant) or gravitational_constant <= 0.0:
        raise ValueError("gravitational_constant must be finite and positive")

    radius = 0.5 * (edges[:-1] + edges[1:])
    annular_area = math.pi * (edges[1:] ** 2 - edges[:-1] ** 2)
    mass = values[:, 0]
    internal_energy = values[:, 1]
    magnetic_b2_volume = values[:, 2]
    radial_momentum = values[:, 3]
    radial_second_moment = values[:, 4]
    radial_gravity_moment = values[:, 5]
    sampled_volume = values[:, 6]

    surface_density = np.full_like(radius, np.nan)
    sound_speed_sq = np.full_like(radius, np.nan)
    alfven_speed_sq = np.full_like(radius, np.nan)
    radial_velocity_mean = np.full_like(radius, np.nan)
    radial_velocity_second = np.full_like(radius, np.nan)
    radial_gravity = np.full_like(radius, np.nan)
    populated = np.isfinite(mass) & (mass > 0.0)
    surface_density[populated] = mass[populated] / annular_area[populated]
    sound_speed_sq[populated] = (
        gamma * (gamma - 1.0) * internal_energy[populated] / mass[populated]
    )
    alfven_speed_sq[populated] = magnetic_b2_volume[populated] / mass[populated]
    radial_velocity_mean[populated] = radial_momentum[populated] / mass[populated]
    radial_velocity_second[populated] = radial_second_moment[populated] / mass[populated]
    radial_gravity[populated] = radial_gravity_moment[populated] / mass[populated]
    radial_dispersion_sq = _nonnegative_variance(
        radial_velocity_second,
        radial_velocity_mean,
    )

    circular_speed_sq = radius * radial_gravity
    circular_speed_sq[circular_speed_sq <= 0.0] = np.nan
    omega_sq = circular_speed_sq / (radius * radius)
    domega_sq_dr = _contiguous_finite_gradient(omega_sq, radius)
    kappa_sq = radius * domega_sq_dr + 4.0 * omega_sq
    kappa_sq[kappa_sq <= 0.0] = np.nan

    def square_root_nonnegative(quantity: np.ndarray) -> np.ndarray:
        result = np.full_like(quantity, np.nan)
        valid = np.isfinite(quantity) & (quantity >= 0.0)
        result[valid] = np.sqrt(quantity[valid])
        return result

    sound_speed = square_root_nonnegative(sound_speed_sq)
    alfven_speed = square_root_nonnegative(alfven_speed_sq)
    radial_dispersion = square_root_nonnegative(radial_dispersion_sq)
    circular_speed = square_root_nonnegative(circular_speed_sq)
    omega = square_root_nonnegative(omega_sq)
    kappa = square_root_nonnegative(kappa_sq)

    denominator = math.pi * gravitational_constant * surface_density

    def q_from_support(support_sq: np.ndarray) -> np.ndarray:
        q = kappa * square_root_nonnegative(support_sq) / denominator
        q[(~np.isfinite(q)) | (q <= 0.0)] = np.nan
        return q

    q_thermal_magnetic = q_from_support(sound_speed_sq + alfven_speed_sq)
    q_thermal_turbulent = q_from_support(sound_speed_sq + radial_dispersion_sq)
    q_all = q_from_support(sound_speed_sq + radial_dispersion_sq + alfven_speed_sq)
    valid = (
        np.isfinite(q_thermal_magnetic)
        & np.isfinite(q_thermal_turbulent)
        & np.isfinite(q_all)
    )

    return {
        "radius": radius,
        "annular_area": annular_area,
        "mass": mass,
        "sampled_volume": sampled_volume,
        "surface_density": surface_density,
        "sound_speed": sound_speed,
        "radial_velocity_mean": radial_velocity_mean,
        "radial_dispersion": radial_dispersion,
        "alfven_speed": alfven_speed,
        "radial_gravity": radial_gravity,
        "circular_speed": circular_speed,
        "omega": omega,
        "kappa": kappa,
        "q_thermal_magnetic": q_thermal_magnetic,
        "q_thermal_turbulent": q_thermal_turbulent,
        "q_thermal_turbulent_magnetic": q_all,
        "valid": valid,
    }


def profile_rows(radial_edges: np.ndarray, profile: dict[str, np.ndarray]) -> list[dict[str, object]]:
    edges_kpc = np.asarray(radial_edges, dtype=np.float64) / KPC_CM
    radius_kpc = profile["radius"] / KPC_CM
    surface_density_msun_pc2 = profile["surface_density"] * PC_CM**2 / MSUN_G
    rows: list[dict[str, object]] = []
    for i in range(len(radius_kpc)):
        rows.append(
            {
                "radius_min_kpc": float(edges_kpc[i]),
                "radius_kpc": float(radius_kpc[i]),
                "radius_max_kpc": float(edges_kpc[i + 1]),
                "q_thermal_magnetic": float(profile["q_thermal_magnetic"][i]),
                "q_thermal_turbulent": float(profile["q_thermal_turbulent"][i]),
                "q_thermal_turbulent_magnetic": float(
                    profile["q_thermal_turbulent_magnetic"][i]
                ),
                "surface_density_msun_pc2": float(surface_density_msun_pc2[i]),
                "sound_speed_km_s": float(profile["sound_speed"][i] / 1.0e5),
                "radial_velocity_mean_km_s": float(
                    profile["radial_velocity_mean"][i] / 1.0e5
                ),
                "radial_dispersion_km_s": float(profile["radial_dispersion"][i] / 1.0e5),
                "alfven_speed_km_s": float(profile["alfven_speed"][i] / 1.0e5),
                "radial_gravity_cm_s2": float(profile["radial_gravity"][i]),
                "circular_speed_km_s": float(profile["circular_speed"][i] / 1.0e5),
                "omega_myr_inv": float(profile["omega"][i] * MYR_S),
                "kappa_myr_inv": float(profile["kappa"][i] * MYR_S),
                "annular_mass_msun": float(profile["mass"][i] / MSUN_G),
                "sampled_volume_kpc3": float(profile["sampled_volume"][i] / KPC_CM**3),
                "valid": bool(profile["valid"][i]),
            }
        )
    return rows


def write_profile_csv(path: Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        raise ValueError("cannot write an empty profile")
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def plot_toomre_profiles(
    path: Path,
    profile: dict[str, np.ndarray],
    *,
    plotfile_name: str,
    time_seconds: float | None,
) -> None:
    radius_kpc = profile["radius"] / KPC_CM
    fig, axis = plt.subplots(figsize=(7.2, 5.2))
    styles = (
        ("q_thermal_magnetic", "Thermal + magnetic", "#0072B2", "-", "o"),
        ("q_thermal_turbulent", "Thermal + turbulent", "#D55E00", "--", "s"),
        (
            "q_thermal_turbulent_magnetic",
            "Thermal + turbulent + magnetic",
            "#009E73",
            "-.",
            "^",
        ),
    )
    for key, label, color, linestyle, marker in styles:
        q_values = np.asarray(profile[key], dtype=np.float64)
        axis.plot(
            radius_kpc,
            np.ma.masked_where((~np.isfinite(q_values)) | (q_values <= 0.0), q_values),
            label=label,
            color=color,
            linestyle=linestyle,
            marker=marker,
            markersize=3.0,
            linewidth=1.6,
        )
    axis.axhline(1.0, color="0.25", linewidth=1.0, linestyle=":", label=r"$Q=1$")
    axis.set_xlabel("Galactocentric radius [kpc]")
    axis.set_ylabel("Gas Toomre Q")
    axis.set_yscale("log")
    axis.set_xlim(float(radius_kpc[0]), float(radius_kpc[-1]))
    axis.grid(True, which="both", alpha=0.2)
    title = f"Gas Toomre Q — {plotfile_name}"
    if time_seconds is not None and math.isfinite(time_seconds):
        title += f"  (t = {time_seconds / MYR_S:.2f} Myr)"
    axis.set_title(title)
    axis.legend(loc="best", frameon=False)
    fig.tight_layout()
    fig.savefig(path, dpi=180)
    plt.close(fig)


def plot_annular_profiles(
    path: Path,
    profile: dict[str, np.ndarray],
    *,
    plotfile_name: str,
    time_seconds: float | None,
) -> None:
    radius_kpc = profile["radius"] / KPC_CM
    surface_density = profile["surface_density"] * PC_CM**2 / MSUN_G
    fig, axes = plt.subplots(
        2,
        1,
        figsize=(7.2, 7.0),
        sharex=True,
        gridspec_kw={"height_ratios": [1.0, 1.25]},
    )

    sigma = np.asarray(surface_density, dtype=np.float64)
    axes[0].plot(
        radius_kpc,
        np.ma.masked_where((~np.isfinite(sigma)) | (sigma <= 0.0), sigma),
        label=r"$\Sigma$",
        color="#0072B2",
        marker="o",
        markersize=3.0,
        linewidth=1.6,
    )
    axes[0].set_ylabel(r"$\Sigma$ [$M_\odot$ pc$^{-2}$]")
    axes[0].set_yscale("log")
    axes[0].grid(True, which="both", alpha=0.2)
    axes[0].legend(loc="best", frameon=False)

    velocity_styles = (
        ("alfven_speed", r"$v_A$", "#CC79A7", "-", "o"),
        ("sound_speed", r"$c_s$", "#009E73", "--", "s"),
        ("circular_speed", r"$v_c$", "#D55E00", "-.", "^"),
        ("radial_dispersion", r"$\delta v_R$", "#56B4E9", ":", "D"),
    )
    for key, label, color, linestyle, marker in velocity_styles:
        values = np.asarray(profile[key], dtype=np.float64) / 1.0e5
        axes[1].plot(
            radius_kpc,
            np.ma.masked_where((~np.isfinite(values)) | (values <= 0.0), values),
            label=label,
            color=color,
            linestyle=linestyle,
            marker=marker,
            markersize=3.0,
            linewidth=1.6,
        )
    axes[1].set_xlabel("Galactocentric radius [kpc]")
    axes[1].set_ylabel("Velocity [km/s]")
    axes[1].set_yscale("log")
    axes[1].set_xlim(float(radius_kpc[0]), float(radius_kpc[-1]))
    axes[1].grid(True, which="both", alpha=0.2)
    axes[1].legend(loc="best", frameon=False, ncols=2)

    title = f"Annular Toomre inputs — {plotfile_name}"
    if time_seconds is not None and math.isfinite(time_seconds):
        title += f"  (t = {time_seconds / MYR_S:.2f} Myr)"
    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(path, dpi=180)
    plt.close(fig)


def _radial_edges(args: argparse.Namespace) -> np.ndarray:
    rmin = float(args.r_min_kpc)
    rmax = float(args.r_max_kpc)
    if not math.isfinite(rmin) or not math.isfinite(rmax) or rmin < 0.0 or rmax <= rmin:
        raise ValueError("radial bounds must be finite, non-negative, and increasing")
    bins = int(args.bins)
    if args.dr_kpc is not None:
        dr = float(args.dr_kpc)
        if not math.isfinite(dr) or dr <= 0.0:
            raise ValueError("dr_kpc must be finite and positive")
        span = rmax - rmin
        full_bins = int(math.floor(span / dr))
        edges_kpc = rmin + dr * np.arange(full_bins + 1, dtype=np.float64)
        if math.isclose(edges_kpc[-1], rmax, rel_tol=1.0e-10, abs_tol=1.0e-10):
            edges_kpc[-1] = rmax
        else:
            edges_kpc = np.append(edges_kpc, rmax)
        if len(edges_kpc) < 4:
            raise ValueError("dr_kpc must produce at least three radial bins")
        return edges_kpc * KPC_CM
    if bins < 3:
        raise ValueError("bins must be at least three to compute epicyclic frequency")
    return np.linspace(rmin * KPC_CM, rmax * KPC_CM, bins + 1)


def _output_directory(input_path: Path, output_dir: Path | None) -> Path:
    if output_dir is not None:
        return output_dir.expanduser().resolve()
    resolved = input_path.expanduser().resolve()
    return (resolved.parent if (resolved / "Header").is_file() else resolved) / "toomre_q"


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input", type=Path, help="Main plotfile or directory containing plt* outputs")
    parser.add_argument("--density")
    parser.add_argument("--momx")
    parser.add_argument("--momy")
    parser.add_argument("--internal-energy", dest="internal_energy")
    parser.add_argument("--bx")
    parser.add_argument("--by")
    parser.add_argument("--bz")
    parser.add_argument("--potential")
    parser.add_argument("--center-kpc", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--r-min-kpc", type=float, default=0.5)
    parser.add_argument("--r-max-kpc", type=float, default=16.0)
    parser.add_argument("--bins", type=int, default=62)
    parser.add_argument("--dr-kpc", type=float)
    parser.add_argument("--z-max-kpc", type=float, default=4.0)
    parser.add_argument("--z-bounds-kpc", nargs=2, type=float, metavar=("ZMIN", "ZMAX"))
    parser.add_argument("--gamma", type=float, default=5.0 / 3.0)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--list-fields", action="store_true")
    parser.add_argument("--progress", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    plotfiles = _find_plotfiles(args.input)
    radial_edges = _radial_edges(args)
    output_dir = _output_directory(args.input, args.output_dir)
    if not args.list_fields:
        output_dir.mkdir(parents=True, exist_ok=True)
    for plotfile in plotfiles:
        stem = f"{plotfile.name}_toomre_q"
        csv_path = output_dir / f"{stem}.csv"
        png_path = output_dir / f"{stem}.png"
        annular_png_path = output_dir / f"{stem}_annular_profiles.png"
        if not args.list_fields and not args.overwrite:
            existing = [str(path) for path in (csv_path, png_path, annular_png_path)
                        if path.exists()]
            if existing:
                raise FileExistsError(
                    "Refusing to overwrite existing output(s): " + ", ".join(existing))
        with amrexplorer.open_plotfile(plotfile) as ds:
            if args.list_fields:
                print(f"[{plotfile}]")
                for index, name in enumerate(ds.fields):
                    print(f"{index:03d} {name}")
                continue
            fields = _resolve_fields(ds, args)
            center = (tuple(float(value) * KPC_CM for value in args.center_kpc)
                      if args.center_kpc is not None else _domain_center(ds))
            if args.z_bounds_kpc is not None:
                z_bounds = tuple(float(value) * KPC_CM for value in args.z_bounds_kpc)
            else:
                zmax = float(args.z_max_kpc) * KPC_CM
                if not math.isfinite(zmax) or zmax <= 0.0:
                    raise ValueError("z_max_kpc must be finite and positive")
                z_bounds = (center[2] - zmax, center[2] + zmax)
            if z_bounds[1] <= z_bounds[0]:
                raise ValueError("z bounds must be increasing")
            print(f"Toomre Q fields for {plotfile.name}: "
                  + ", ".join(f"{role}={name}" for role, name in fields.items()),
                  file=sys.stderr, flush=True)
            moments = annular_moments(ds, fields, radial_edges,
                                      (float(z_bounds[0]), float(z_bounds[1])),
                                      center, progress=args.progress)
            time_seconds = ds.time
        profile = derive_toomre_profiles(radial_edges, moments, gamma=float(args.gamma))
        rows = profile_rows(radial_edges, profile)
        write_profile_csv(csv_path, rows)
        plot_toomre_profiles(png_path, profile, plotfile_name=plotfile.name,
                             time_seconds=time_seconds)
        plot_annular_profiles(annular_png_path, profile, plotfile_name=plotfile.name,
                              time_seconds=time_seconds)
        print(f"wrote {png_path}")
        print(f"wrote {annular_png_path}")
        print(f"wrote {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
