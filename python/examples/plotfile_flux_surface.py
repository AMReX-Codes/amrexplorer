#!/usr/bin/env python3
"""Integrate fluxes through origin-centered spherical surfaces.

A port of Kangaroo's ``scripts/plotfile_flux_surface.py`` to amrexplorer's
exact leaf-cell traversal (docs/analysis-scripting-interface.md). The
arguments, field inference, and JSON output are Kangaroo's, unchanged.

For every radius, each leaf cell (a cell no finer level covers) that the
sphere meets contributes with the area of the sphere's tangent plane inside
it (``surfaces.sphere_area_in_cells``, Kangaroo's geometry). Its fluxes,
from the cell-center state as in Kangaroo's
``flux_surface_integral_accumulate`` kernel, are

    mass        rho v_r A
    hydro       (e_hydro + p_gas) v_r A
    MHD         ((E + p_gas + e_mag) v_r - (v . B)(rhat . B)) A
    scalar      scalar_density v_r A

with e_mag = B^2 / 2, e_hydro = E - e_mag, and p_gas = (gamma - 1)(e_hydro -
kinetic). Cells whose center is at the origin, whose density is not finite
and positive, with any non-finite input, or whose v_r or p_gas is not finite
are skipped. Each component is binned by its own sign (negative if < 0) and,
with ``--temperature-bins``, by temperature: bins are [lower, upper) with the
last edge closed, and cells outside the edges are skipped. The plotfile is
assumed to be in cgs with B in Gaussian units scaled so that the magnetic
energy density is B^2 / 2, as Quokka writes it; the backend itself never
converts units.

Validates: analysis totals.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from typing import Iterable

import numpy as np

import amrexplorer
from amrexplorer import surfaces

COMPONENTS = (
    "mass_flux_sphere",
    "hydro_energy_flux_sphere",
    "mhd_energy_flux_sphere",
    "passive_scalar_flux_sphere",
)
SIGN_BINS = ("negative", "positive")
PC_CM = 3.0856775814913673e18
MSUN_G = 1.98847e33
YR_S = 365.25 * 24.0 * 3600.0
CENTER = (0.0, 0.0, 0.0)

FIELD_CANDIDATES = {
    "density": ("density", "rho", "gasDensity", "Density"),
    "momx": ("xmom", "momx", "x1Momentum", "x-GasMomentum", "momentum_x", "gasMomentum_x"),
    "momy": ("ymom", "momy", "x2Momentum", "y-GasMomentum", "momentum_y", "gasMomentum_y"),
    "momz": ("zmom", "momz", "x3Momentum", "z-GasMomentum", "momentum_z", "gasMomentum_z"),
    "energy": ("energy", "Egas", "gasEnergy", "GasEnergy", "totalEnergy", "Energy"),
    "scalar": ("scalar_0", "scalar0", "Scalar0", "passive_scalar", "passiveScalar", "scalar"),
    "bx": ("x-BField", "Bx", "bx", "magx", "magnetic_x", "B_x", "cell_centered_Bx"),
    "by": ("y-BField", "By", "by", "magy", "magnetic_y", "B_y", "cell_centered_By"),
    "bz": ("z-BField", "Bz", "bz", "magz", "magnetic_z", "B_z", "cell_centered_Bz"),
}
TEMPERATURE_CANDIDATES = ("temperature", "Temperature", "gasTemperature", "gas_temperature")


def _parse_field_arg(value: str | None) -> str | None:
    if value is None:
        return None
    value = value.strip()
    return value or None


def _pick_field(
    role: str,
    explicit: str | None,
    available: Iterable[str],
    *,
    candidates: Iterable[str] | None = None,
) -> str:
    names = list(available)
    name_set = set(names)
    if explicit:
        if explicit not in name_set:
            raise RuntimeError(
                f"Field {explicit!r} for {role!r} is not listed in plotfile metadata. "
                f"Use --list-fields; available fields include: {', '.join(names[:40])}"
            )
        return explicit
    candidate_names = FIELD_CANDIDATES[role] if candidates is None else tuple(candidates)
    for candidate in candidate_names:
        if candidate in name_set:
            return candidate
    raise RuntimeError(
        f"Could not infer field for {role!r}. "
        f"Pass --{role}; available fields include: {', '.join(names[:40])}"
    )


def _finite_radius(value: float) -> float:
    radius = float(value)
    if not math.isfinite(radius) or radius <= 0.0:
        raise ValueError("radius must be finite and positive")
    return radius


def _finite_radii(values: Iterable[float]) -> np.ndarray:
    radii = np.asarray([float(value) for value in values], dtype=np.float64)
    if radii.size == 0 or not np.all(np.isfinite(radii)) or np.any(radii <= 0.0):
        raise ValueError("radii must be finite and positive")
    return radii


def _finite_temperature_bins(values: Iterable[float]) -> np.ndarray:
    bins = np.asarray([float(value) for value in values], dtype=np.float64)
    if bins.size < 2:
        raise ValueError("temperature bins must contain at least two edges")
    if not np.all(np.isfinite(bins)):
        raise ValueError("temperature bins must be finite")
    if np.any(np.diff(bins) <= 0.0):
        raise ValueError("temperature bins must be strictly increasing")
    return bins


def _parse_temperature_bins(values: Iterable[str]) -> np.ndarray:
    edges: list[float] = []
    for value in values:
        for token in str(value).split(","):
            token = token.strip()
            if token:
                edges.append(float(token))
    return _finite_temperature_bins(edges)


def _require_radii_intersect_blocks(ds, radii: np.ndarray) -> None:
    """Kangaroo refuses a radius whose sphere lies in no grid's radial range."""

    bounds = []
    for block in ds.blocks():
        lo2 = 0.0
        hi2 = 0.0
        for x0, x1 in zip(block["lower"], block["upper"]):
            if x1 < 0.0:
                lo2 += x1 * x1
            elif x0 > 0.0:
                lo2 += x0 * x0
            hi2 += max(abs(x0), abs(x1)) ** 2
        bounds.append((lo2, hi2))
    for radius in radii:
        radius2 = float(radius) * float(radius)
        if not any(lo2 <= radius2 <= hi2 for lo2, hi2 in bounds):
            raise ValueError("radius does not intersect any mesh block")


def _temperature_bin(temperature: np.ndarray, edges: np.ndarray) -> np.ndarray:
    """std::upper_bound binning with the last edge closed; -1 is out of range."""

    upper = np.searchsorted(edges, temperature, side="right")
    index = upper - 1
    outside = (upper == 0) | (upper == len(edges))
    index = np.where(outside, -1, index)
    return np.where(temperature == edges[-1], len(edges) - 2, index)


def flux_surface_integral(ds, fields: dict, radii, *, gamma: float,
                          temperature_bins=None, progress: bool = False):
    """Signed fluxes through each sphere, shape (num_radii, 2, num_temperature
    bins, 4): sign bin (negative, positive), temperature bin (one when
    unbinned), component in COMPONENTS order, in native units."""

    radii = np.asarray(radii, dtype=np.float64)
    use_temperature = temperature_bins is not None
    num_temperature_bins = len(temperature_bins) - 1 if use_temperature else 1
    names = [fields[role] for role in FIELD_CANDIDATES]
    if use_temperature:
        names.append(fields["temperature"])
    names = list(dict.fromkeys(names))
    levels = ds.levels
    gamma_minus_one = float(gamma) - 1.0
    values = np.zeros((len(radii), 2, num_temperature_bins, len(COMPONENTS)))

    for radius_idx, radius in enumerate(radii):
        radius = float(radius)
        region = surfaces.sphere_region(ds, CENTER, radius)

        def report(done, total):
            if progress:
                print(f"\rradius {radius_idx + 1}/{len(radii)}: chunks {done}/{total}",
                      end="", file=sys.stderr, flush=True)

        for chunk in ds.leaf_cells(names, region=region, dtype=np.float64,
                                   progress=report):
            section = surfaces.sphere_area_in_cells(chunk, CENTER, radius)
            area = section["area"]
            hit = area > 0.0
            if not np.any(hit):
                continue
            # Cell centers as Kangaroo forms them, from the cell's edges.
            level = levels[chunk.level]
            center = []
            for axis, index in enumerate(chunk.indices()):
                lo = level["index_origin"][axis] + index[hit].astype(np.float64) * level["cell_size"][axis]
                center.append(0.5 * (lo + (lo + level["cell_size"][axis])))
            x, y, z = center
            area = area[hit]
            rho = chunk[fields["density"]][hit]
            momx = chunk[fields["momx"]][hit]
            momy = chunk[fields["momy"]][hit]
            momz = chunk[fields["momz"]][hit]
            energy = chunk[fields["energy"]][hit]
            scalar = chunk[fields["scalar"]][hit]
            bx = chunk[fields["bx"]][hit]
            by = chunk[fields["by"]][hit]
            bz = chunk[fields["bz"]][hit]

            r = np.sqrt(x * x + y * y + z * z)
            keep = (r > 0.0) & (rho > 0.0) & np.isfinite(rho)
            for array in (momx, momy, momz, energy, scalar, bx, by, bz):
                keep &= np.isfinite(array)
            if use_temperature:
                temperature = chunk[fields["temperature"]][hit]
                temperature_bin = _temperature_bin(temperature, temperature_bins)
                keep &= np.isfinite(temperature) & (temperature_bin >= 0)
            else:
                temperature_bin = np.zeros(rho.shape, dtype=np.int64)

            with np.errstate(all="ignore"):
                vx = momx / rho
                vy = momy / rho
                vz = momz / rho
                vr = (x * momx + y * momy + z * momz) / (rho * r)
                rhat_x = x / r
                rhat_y = y / r
                rhat_z = z / r
                kinetic = 0.5 * (momx * momx + momy * momy + momz * momz) / rho
                emag = 0.5 * (bx * bx + by * by + bz * bz)
                ehydro = energy - emag
                pgas = gamma_minus_one * (ehydro - kinetic)
                keep &= np.isfinite(vr) & np.isfinite(pgas)

                bdotv = vx * bx + vy * by + vz * bz
                br = rhat_x * bx + rhat_y * by + rhat_z * bz
                fluxes = (
                    rho * vr * area,
                    (ehydro + pgas) * vr * area,
                    ((energy + pgas + emag) * vr - bdotv * br) * area,
                    scalar * vr * area,
                )
            for component, flux in enumerate(fluxes):
                flux = flux[keep]
                sign_bin = np.where(flux < 0.0, 0, 1)
                np.add.at(values[radius_idx, :, :, component],
                          (sign_bin, temperature_bin[keep]), flux)
        if progress:
            print(file=sys.stderr)
    return values


def _flux_rows_and_derived(
    radii: np.ndarray,
    values: np.ndarray,
    *,
    pc_cm: float,
    temperature_bins: np.ndarray | None = None,
) -> tuple[list[dict], dict]:
    values_by_temperature = np.asarray(values, dtype=np.float64)
    temperature_edges = temperature_bins
    sign_summed_values = values_by_temperature.sum(axis=2)
    summed_values = sign_summed_values.sum(axis=1)

    def temperature_bin_rows(radius_idx: int, sign_idx: int) -> list[dict]:
        if temperature_edges is None:
            return []
        return [
            {
                "temperature_min": float(temperature_edges[temp_idx]),
                "temperature_max": float(temperature_edges[temp_idx + 1]),
                "fluxes": {
                    name: float(values_by_temperature[radius_idx, sign_idx, temp_idx, j])
                    for j, name in enumerate(COMPONENTS)
                },
            }
            for temp_idx in range(len(temperature_edges) - 1)
        ]

    flux_rows = [
        {
            "radius": float(radii[i]),
            "radius_kpc": float(radii[i] / (1.0e3 * pc_cm)),
            "fluxes": {name: float(summed_values[i, j]) for j, name in enumerate(COMPONENTS)},
            "flux_bins": {
                sign: {
                    name: float(sign_summed_values[i, sign_idx, j])
                    for j, name in enumerate(COMPONENTS)
                }
                for sign_idx, sign in enumerate(SIGN_BINS)
            },
            "flux_bins_by_temperature": (
                {
                    sign: temperature_bin_rows(i, sign_idx)
                    for sign_idx, sign in enumerate(SIGN_BINS)
                }
                if temperature_edges is not None
                else None
            ),
        }
        for i in range(len(radii))
    ]
    derived = {
        "mass_flux_msun_per_yr": (
            float(summed_values[0, 0] * YR_S / MSUN_G) if len(radii) == 1 else None
        ),
        "mass_flux_msun_per_yr_bins": (
            {
                sign: float(sign_summed_values[0, sign_idx, 0] * YR_S / MSUN_G)
                for sign_idx, sign in enumerate(SIGN_BINS)
            }
            if len(radii) == 1
            else None
        ),
        "mass_flux_msun_per_yr_bins_by_temperature": (
            {
                sign: [
                    {
                        "temperature_min": float(temperature_edges[temp_idx]),
                        "temperature_max": float(temperature_edges[temp_idx + 1]),
                        "mass_flux_msun_per_yr": float(
                            values_by_temperature[0, sign_idx, temp_idx, 0] * YR_S / MSUN_G
                        ),
                    }
                    for temp_idx in range(len(temperature_edges) - 1)
                ]
                for sign_idx, sign in enumerate(SIGN_BINS)
            }
            if len(radii) == 1 and temperature_edges is not None
            else None
        ),
        "mass_flux_msun_per_yr_by_radius": [
            {
                "radius": float(radii[i]),
                "radius_kpc": float(radii[i] / (1.0e3 * pc_cm)),
                "mass_flux_msun_per_yr": float(summed_values[i, 0] * YR_S / MSUN_G),
                "mass_flux_msun_per_yr_bins": {
                    sign: float(sign_summed_values[i, sign_idx, 0] * YR_S / MSUN_G)
                    for sign_idx, sign in enumerate(SIGN_BINS)
                },
                "mass_flux_msun_per_yr_bins_by_temperature": (
                    {
                        sign: [
                            {
                                "temperature_min": float(temperature_edges[temp_idx]),
                                "temperature_max": float(temperature_edges[temp_idx + 1]),
                                "mass_flux_msun_per_yr": float(
                                    values_by_temperature[i, sign_idx, temp_idx, 0]
                                    * YR_S
                                    / MSUN_G
                                ),
                            }
                            for temp_idx in range(len(temperature_edges) - 1)
                        ]
                        for sign_idx, sign in enumerate(SIGN_BINS)
                    }
                    if temperature_edges is not None
                    else None
                ),
            }
            for i in range(len(radii))
        ],
    }
    return flux_rows, derived


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Integrate fluxes through origin-centered spheres in an AMReX plotfile."
    )
    p.add_argument("plotfile")
    p.add_argument("--radius", type=float, help="Sphere radius in plotfile coordinate units.")
    p.add_argument("--radius-kpc", type=float, help="Sphere radius in kpc; converted to cm.")
    p.add_argument(
        "--rmin-kpc",
        "--rmin_kpc",
        dest="rmin_kpc",
        type=float,
        help="Minimum sphere radius in kpc.",
    )
    p.add_argument(
        "--rmax-kpc",
        "--rmax_kpc",
        dest="rmax_kpc",
        type=float,
        help="Maximum sphere radius in kpc.",
    )
    p.add_argument(
        "--nbins",
        dest="nbins",
        type=int,
        default=64,
        help="Number of log-spaced radii for --rmin-kpc/--rmax-kpc.",
    )
    p.add_argument("--density")
    p.add_argument("--momx")
    p.add_argument("--momy")
    p.add_argument("--momz")
    p.add_argument("--energy")
    p.add_argument("--scalar")
    p.add_argument("--bx")
    p.add_argument("--by")
    p.add_argument("--bz")
    p.add_argument("--temperature")
    p.add_argument(
        "--temperature-bins",
        nargs="+",
        help=(
            "Temperature bin edges, separated by spaces or commas. "
            "Requires the plotfile temperature field."
        ),
    )
    p.add_argument("--gamma", type=float, default=5.0 / 3.0)
    p.add_argument("--output-json", help="Write flux values and field bindings to this JSON file.")
    p.add_argument("--list-fields", action="store_true", help="Print plotfile field names and exit.")
    p.add_argument("--progress", action="store_true", help="Show per-radius chunk progress.")
    p.add_argument(
        "--cache-mb", type=int, default=None,
        help="Block-cache budget in MiB (default: AMREXPLORER_CACHE_SIZE_MB or 1 GiB).",
    )
    a = p.parse_args(argv)

    cache = None if a.cache_mb is None else a.cache_mb * 1024 * 1024
    with amrexplorer.open_plotfile(a.plotfile, cache_bytes=cache) as ds:
        available = ds.fields

        if a.list_fields:
            for idx, name in enumerate(available):
                print(f"{idx:03d} {name}")
            return 0

        if ds.dimension != 3:
            raise RuntimeError("spherical flux surfaces need a 3-D plotfile")

        pc_cm = PC_CM
        single_radius_count = sum(value is not None for value in (a.radius, a.radius_kpc))
        range_radius_count = sum(value is not None for value in (a.rmin_kpc, a.rmax_kpc))
        if single_radius_count == 0 and range_radius_count == 0:
            raise RuntimeError("Pass --radius, --radius-kpc, or both --rmin-kpc and --rmax-kpc")
        if single_radius_count > 0 and range_radius_count > 0:
            raise RuntimeError("Pass either a single radius or --rmin-kpc/--rmax-kpc, not both")
        if single_radius_count > 1:
            raise RuntimeError("Pass only one of --radius or --radius-kpc")
        if range_radius_count not in (0, 2):
            raise RuntimeError("Pass both --rmin-kpc and --rmax-kpc")

        if range_radius_count == 2:
            if int(a.nbins) <= 0:
                raise ValueError("nbins must be positive")
            rmin_kpc = _finite_radius(a.rmin_kpc)
            rmax_kpc = _finite_radius(a.rmax_kpc)
            if rmin_kpc > rmax_kpc:
                raise ValueError("rmin_kpc must be less than or equal to rmax_kpc")
            radii_kpc = _finite_radii(
                np.logspace(np.log10(rmin_kpc), np.log10(rmax_kpc), int(a.nbins))
            )
            radii = _finite_radii(radii_kpc * 1.0e3 * pc_cm)
        else:
            radius = _finite_radius(
                a.radius if a.radius is not None else float(a.radius_kpc) * 1.0e3 * pc_cm
            )
            radii = _finite_radii([radius])
        temperature_bins = (
            _parse_temperature_bins(a.temperature_bins)
            if a.temperature_bins is not None
            else None
        )
        gamma = float(a.gamma)
        if not math.isfinite(gamma) or gamma <= 1.0:
            raise ValueError("gamma must be finite and greater than 1")

        fields: dict[str, str] = {}
        for role in FIELD_CANDIDATES:
            fields[role] = _pick_field(role, _parse_field_arg(getattr(a, role)), available)
        if temperature_bins is not None:
            fields["temperature"] = _pick_field(
                "temperature",
                _parse_field_arg(a.temperature),
                available,
                candidates=TEMPERATURE_CANDIDATES,
            )

        print(
            "flux surface fields: "
            + ", ".join(f"{role}={name}" for role, name in fields.items()),
            file=sys.stderr,
            flush=True,
        )
        _require_radii_intersect_blocks(ds, radii)

        values = flux_surface_integral(
            ds, fields, radii, gamma=gamma, temperature_bins=temperature_bins,
            progress=bool(a.progress))
        flux_rows, derived = _flux_rows_and_derived(
            radii,
            values,
            pc_cm=pc_cm,
            temperature_bins=temperature_bins,
        )
        result = {
            "plotfile": a.plotfile,
            "time": float(ds.time),
            "radius": float(radii[0]) if len(radii) == 1 else None,
            "radius_kpc": float(radii[0] / (1.0e3 * pc_cm)) if len(radii) == 1 else None,
            "radii": [float(radius) for radius in radii],
            "radii_kpc": [float(radius / (1.0e3 * pc_cm)) for radius in radii],
            "nbins": int(len(radii)),
            "temperature_bins": (
                [float(edge) for edge in temperature_bins]
                if temperature_bins is not None
                else None
            ),
            "gamma": gamma,
            "fields": dict(fields),
            "fluxes": flux_rows[0]["fluxes"] if len(radii) == 1 else None,
            "flux_bins": flux_rows[0]["flux_bins"] if len(radii) == 1 else None,
            "flux_bins_by_temperature": (
                flux_rows[0]["flux_bins_by_temperature"]
                if len(radii) == 1 and temperature_bins is not None
                else None
            ),
            "fluxes_by_radius": flux_rows,
            "derived": derived,
        }

        print(json.dumps(result, indent=2, sort_keys=True))
        if a.output_json:
            with open(a.output_json, "w", encoding="utf-8") as f:
                json.dump(result, f, indent=2, sort_keys=True)
                f.write("\n")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
