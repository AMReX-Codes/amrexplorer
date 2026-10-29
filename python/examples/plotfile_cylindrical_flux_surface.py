#!/usr/bin/env python3
"""Integrate fluxes through origin-centered, z-aligned cylinders in a plotfile.

A port of Kangaroo's ``scripts/plotfile_cylindrical_flux_surface.py`` to
amrexplorer's leaf-cell traversal and surface helpers
(docs/analysis-scripting-interface.md). The arguments and the JSON output are
Kangaroo's, unchanged.

Use --radius-kpc and --height-kpc for one cylinder. For a one-dimensional
profile, either vary half-height with --zmin-kpc/--zmax-kpc at fixed radius or
vary radius with --rmin-kpc/--rmax-kpc at fixed half-height (log-spaced,
--nbins values). Each leaf cell the surface meets contributes its state times
the area the surface has in it (``surfaces.cylinder_area_in_cells``:
Kangaroo's tangent-plane wall with normal (x, y, 0)/R at the cell center, and
exact disk-rectangle end caps with normals +z and -z and their first moments;
a cap on a cell face belongs to the interior cell). Per cell and section the
script forms Kangaroo's eight fluxes: mass, hydro and MHD energy, passive
scalar, advective and Maxwell radial L_z flux (lever arm = the cylinder
radius; nonzero on the wall only), and advective and Maxwell vertical L_z flux
(lever arm integrated over the cap footprint; nonzero on the caps only). Each
flux is binned by its own sign (not the gas velocity), by geometric section
(walls, endcaps) and optionally by temperature. Cells with rho <= 0, any
non-finite input, or a non-finite normal velocity or gas pressure are
skipped, as in Kangaroo's ``cylindrical_flux_surface_integral_accumulate``.

Magnetic fields must use B**2/2 energy normalization. The plotfile is assumed
to be in cgs, as Quokka writes it (angular-momentum fluxes are then in
g cm**2 s**-2); the backend itself never converts units.

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

PC_CM = 3.0856775814913673e18
MSUN_G = 1.98847e33
YR_S = 365.25 * 24.0 * 3600.0
CENTER = (0.0, 0.0, 0.0)

COMPONENTS = (
    "mass_flux_cylinder",
    "hydro_energy_flux_cylinder",
    "mhd_energy_flux_cylinder",
    "passive_scalar_flux_cylinder",
    "advective_radial_angular_momentum_flux_cylinder",
    "maxwell_radial_angular_momentum_flux_cylinder",
    "advective_vertical_angular_momentum_flux_cylinder",
    "maxwell_vertical_angular_momentum_flux_cylinder",
)
SIGN_BINS = ("negative", "positive")
GEOMETRIC_SECTIONS = ("endcaps", "walls")
ENDCAPS, WALLS = 0, 1

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


# Input checks and field inference (Kangaroo's, unchanged) --------------------

def _parse_field_arg(value: str | None) -> str | None:
    if value is None:
        return None
    value = value.strip()
    return value or None


def _pick_field(role: str, explicit: str | None, available: Iterable[str], *,
                candidates: Iterable[str] | None = None) -> str:
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


def _finite_heights(values: Iterable[float]) -> np.ndarray:
    heights = np.asarray([float(value) for value in values], dtype=np.float64)
    if heights.size == 0 or not np.all(np.isfinite(heights)) or np.any(heights <= 0.0):
        raise ValueError("heights must be finite and positive")
    return heights


def _parse_temperature_bins(values: Iterable[str]) -> np.ndarray:
    edges: list[float] = []
    for value in values:
        for token in str(value).split(","):
            token = token.strip()
            if token:
                edges.append(float(token))
    bins = np.asarray(edges, dtype=np.float64)
    if bins.size < 2:
        raise ValueError("temperature bins must contain at least two edges")
    if not np.all(np.isfinite(bins)):
        raise ValueError("temperature bins must be finite")
    if np.any(np.diff(bins) <= 0.0):
        raise ValueError("temperature bins must be strictly increasing")
    return bins


def _scan(single, single_kpc, range_min_kpc, range_max_kpc, nbins, *, pc_cm,
          name, single_flags, range_flags, finite_values):
    """Kangaroo's selection of one value or a log-spaced range, in cm.

    kpc converts as Kangaroo writes it, ``value * 1.0e3 * pc_cm`` evaluated
    left to right: ``value * (1.0e3 * pc_cm)`` can differ by an ulp, which
    moves a surface on or off a cell face or corner."""
    single_count = sum(value is not None for value in (single, single_kpc))
    range_count = sum(value is not None for value in (range_min_kpc, range_max_kpc))
    if single_count == 0 and range_count == 0:
        raise RuntimeError(f"Pass {single_flags[0]}, {single_flags[1]}, or both "
                           f"{range_flags[0]} and {range_flags[1]}")
    if single_count > 0 and range_count > 0:
        raise RuntimeError(f"Pass either a single {name} or "
                           f"{range_flags[0]}/{range_flags[1]}, not both")
    if single_count > 1:
        raise RuntimeError(f"Pass only one of {single_flags[0]} or {single_flags[1]}")
    if range_count not in (0, 2):
        raise RuntimeError(f"Pass both {range_flags[0]} and {range_flags[1]}")
    if range_count == 2:
        if int(nbins) <= 0:
            raise ValueError("nbins must be positive")
        low = _finite_radius(range_min_kpc)
        high = _finite_radius(range_max_kpc)
        if low > high:
            raise ValueError(f"{range_flags[0][2:].replace('-', '_')} must be less "
                             f"than or equal to {range_flags[1][2:].replace('-', '_')}")
        values_kpc = finite_values(np.logspace(np.log10(low), np.log10(high), int(nbins)))
        return finite_values(values_kpc * 1.0e3 * pc_cm)
    value = _finite_radius(single if single is not None else float(single_kpc) * 1.0e3 * pc_cm)
    return finite_values([value])


# Accumulation ----------------------------------------------------------------

def _accumulate_section(out, state, area, nx, ny, nz, section, radius,
                        cap_x, cap_y, gamma_minus_one, temperature_edges):
    """Add one section's fluxes for the cells with area > 0 to
    out[sign, temperature_bin, section, component], per cell as Kangaroo's
    accumulate_flux lambda does it."""
    hit = area > 0.0
    if not np.any(hit):
        return
    s = {name: values[hit] for name, values in state.items()}
    area = area[hit]
    nx, ny, nz = (v[hit] if isinstance(v, np.ndarray) else v for v in (nx, ny, nz))
    cap_x, cap_y = (v[hit] if isinstance(v, np.ndarray) else v for v in (cap_x, cap_y))
    rho, momx, momy, momz = s["density"], s["momx"], s["momy"], s["momz"]
    energy, scalar, bx, by, bz = s["energy"], s["scalar"], s["bx"], s["by"], s["bz"]

    with np.errstate(all="ignore"):
        keep = (rho > 0.0) & np.isfinite(rho)
        for values in (momx, momy, momz, energy, scalar, bx, by, bz):
            keep &= np.isfinite(values)
        num_temperature_bins = 1
        temperature_bin = np.zeros(rho.shape, dtype=np.int64)
        if temperature_edges is not None:
            temperature = s["temperature"]
            keep &= np.isfinite(temperature)
            num_temperature_bins = len(temperature_edges) - 1
            # std::upper_bound; values off either end are dropped, except
            # exactly the last edge, which goes to the last bin.
            upper = np.searchsorted(temperature_edges, temperature, side="right")
            inside = (upper > 0) & (upper < len(temperature_edges))
            last = temperature == temperature_edges[-1]
            keep &= inside | last
            temperature_bin = np.where(inside, upper - 1, num_temperature_bins - 1)

        vx = momx / rho
        vy = momy / rho
        vz = momz / rho
        vnormal = vx * nx + vy * ny + vz * nz
        kinetic = 0.5 * (momx * momx + momy * momy + momz * momz) / rho
        emag = 0.5 * (bx * bx + by * by + bz * bz)
        ehydro = energy - emag
        pgas = gamma_minus_one * (ehydro - kinetic)
        keep &= np.isfinite(vnormal) & np.isfinite(pgas)

        bdotv = vx * bx + vy * by + vz * bz
        bnormal = nx * bx + ny * by + nz * bz
        fluxes = (
            rho * vnormal * area,
            (ehydro + pgas) * vnormal * area,
            ((energy + pgas + emag) * vnormal - bdotv * bnormal) * area,
            scalar * vnormal * area,
            radius * (nx * momy - ny * momx) * vnormal * area,
            -radius * (nx * by - ny * bx) * bnormal * area,
            (cap_x * momy - cap_y * momx) * vnormal,
            -(cap_x * by - cap_y * bx) * bnormal,
        )
    if not np.any(keep):
        return
    temperature_bin = temperature_bin[keep]
    for component, flux in enumerate(fluxes):
        flux = np.broadcast_to(flux, keep.shape)[keep]
        slot = np.where(flux < 0.0, 0, 1) * num_temperature_bins + temperature_bin
        sums = np.bincount(slot, weights=flux, minlength=2 * num_temperature_bins)
        out[:, :, section, component] += sums.reshape(2, num_temperature_bins)


def cylindrical_flux_surface_integral(ds, fields, radii, heights, *, gamma,
                                      temperature_bins=None, progress=False):
    """Per surface: fluxes of shape (2 sign bins, temperature bins, 2 sections,
    8 components). A scan varies the radius or the half-height; the other is
    shared by every surface."""
    if len(radii) > 1 and len(heights) > 1:
        raise ValueError("radius and height cannot both contain multiple values")
    if not math.isfinite(gamma) or gamma <= 1.0:
        raise ValueError("gamma must be finite and greater than 1")
    count = max(len(radii), len(heights))
    surfaces_rh = [(float(radii[i if len(radii) > 1 else 0]),
                    float(heights[i if len(heights) > 1 else 0])) for i in range(count)]
    num_temperature_bins = 1 if temperature_bins is None else len(temperature_bins) - 1
    values = np.zeros((count, 2, num_temperature_bins, len(GEOMETRIC_SECTIONS),
                       len(COMPONENTS)))
    # One pass over the leaf cells of the largest cylinder's padded region,
    # which contains every smaller one's.
    region = surfaces.cylinder_region(ds, CENTER, float(np.max(radii)),
                                      float(np.max(heights)))
    roles = list(fields)
    report = None
    if progress:
        def report(done, total):
            print(f"\rchunks {done}/{total}", end="", file=sys.stderr, flush=True)
    for chunk in ds.leaf_cells([fields[role] for role in roles], region=region,
                               dtype=np.float64, progress=report):
        for surface, (radius, height) in enumerate(surfaces_rh):
            section = surfaces.cylinder_area_in_cells(chunk, CENTER, radius, height)
            hit = ((section["wall_area"] > 0.0) | (section["upper_cap_area"] > 0.0)
                   | (section["lower_cap_area"] > 0.0))
            if not np.any(hit):
                continue
            state = {role: np.asarray(chunk[fields[role]])[hit] for role in roles}
            geometry = {key: np.asarray(column)[hit] for key, column in section.items()}
            out = values[surface]
            cap_x, cap_y = geometry["cap_moment_x"], geometry["cap_moment_y"]
            _accumulate_section(out, state, geometry["wall_area"],
                                geometry["wall_normal_x"], geometry["wall_normal_y"],
                                0.0, WALLS, radius, 0.0, 0.0, gamma - 1.0,
                                temperature_bins)
            _accumulate_section(out, state, geometry["upper_cap_area"], 0.0, 0.0, 1.0,
                                ENDCAPS, radius, cap_x, cap_y, gamma - 1.0,
                                temperature_bins)
            _accumulate_section(out, state, geometry["lower_cap_area"], 0.0, 0.0, -1.0,
                                ENDCAPS, radius, cap_x, cap_y, gamma - 1.0,
                                temperature_bins)
    if progress:
        print(file=sys.stderr)
    return values


# JSON assembly (Kangaroo's, unchanged) ---------------------------------------

def _flux_rows_and_derived(heights, values, *, pc_cm, temperature_bins=None):
    """`values` has shape (len(heights), 2, temperature bins, 2, 8)."""
    values_by_temperature_section = np.asarray(values, dtype=np.float64)
    temperature_edges = temperature_bins
    values_by_temperature = values_by_temperature_section.sum(axis=3)
    sign_summed_values = values_by_temperature.sum(axis=2)
    summed_values = sign_summed_values.sum(axis=1)
    section_summed_values = values_by_temperature_section.sum(axis=2)
    kpc = 1.0e3 * pc_cm

    def temperature_bin_rows(height_idx: int, sign_idx: int) -> list[dict]:
        if temperature_edges is None:
            return []
        return [
            {
                "temperature_min": float(temperature_edges[temp_idx]),
                "temperature_max": float(temperature_edges[temp_idx + 1]),
                "fluxes": {
                    name: float(values_by_temperature[height_idx, sign_idx, temp_idx, j])
                    for j, name in enumerate(COMPONENTS)
                },
                "fluxes_by_geometric_section": {
                    section: {
                        name: float(values_by_temperature_section[
                            height_idx, sign_idx, temp_idx, section_idx, j])
                        for j, name in enumerate(COMPONENTS)
                    }
                    for section_idx, section in enumerate(GEOMETRIC_SECTIONS)
                },
            }
            for temp_idx in range(len(temperature_edges) - 1)
        ]

    flux_rows = [
        {
            "height": float(heights[i]),
            "height_kpc": float(heights[i] / kpc),
            "z_min": float(-heights[i]),
            "z_max": float(heights[i]),
            "fluxes": {name: float(summed_values[i, j]) for j, name in enumerate(COMPONENTS)},
            "flux_bins": {
                sign: {name: float(sign_summed_values[i, sign_idx, j])
                       for j, name in enumerate(COMPONENTS)}
                for sign_idx, sign in enumerate(SIGN_BINS)
            },
            "flux_bins_by_geometric_section": {
                sign: {
                    section: {name: float(section_summed_values[i, sign_idx, section_idx, j])
                              for j, name in enumerate(COMPONENTS)}
                    for section_idx, section in enumerate(GEOMETRIC_SECTIONS)
                }
                for sign_idx, sign in enumerate(SIGN_BINS)
            },
            "flux_bins_by_temperature": (
                {sign: temperature_bin_rows(i, sign_idx)
                 for sign_idx, sign in enumerate(SIGN_BINS)}
                if temperature_edges is not None else None
            ),
        }
        for i in range(len(heights))
    ]
    # Sum both component sign bins before forming the net total. Opposite
    # angular-momentum signs need not correspond to inward/outward gas.
    net_by_section = section_summed_values.sum(axis=1)

    def angular_momentum_rows(first: int) -> list[dict]:
        return [
            {
                "height": float(heights[i]),
                "height_kpc": float(heights[i] / kpc),
                "by_geometric_section": {
                    section: {
                        "advective": float(net_by_section[i, section_idx, first]),
                        "maxwell": float(net_by_section[i, section_idx, first + 1]),
                        "total": float(net_by_section[i, section_idx, first:first + 2].sum()),
                    }
                    for section_idx, section in enumerate(GEOMETRIC_SECTIONS)
                },
            }
            for i in range(len(heights))
        ]

    derived = {
        "radial_angular_momentum_flux_by_height": angular_momentum_rows(4),
        "vertical_angular_momentum_flux_by_height": angular_momentum_rows(6),
        "mass_flux_msun_per_yr": (
            float(summed_values[0, 0] * YR_S / MSUN_G) if len(heights) == 1 else None
        ),
        "mass_flux_msun_per_yr_by_height": [
            {
                "height": float(heights[i]),
                "height_kpc": float(heights[i] / kpc),
                "mass_flux_msun_per_yr": float(summed_values[i, 0] * YR_S / MSUN_G),
                "mass_flux_msun_per_yr_bins": {
                    sign: float(sign_summed_values[i, sign_idx, 0] * YR_S / MSUN_G)
                    for sign_idx, sign in enumerate(SIGN_BINS)
                },
                "mass_flux_msun_per_yr_bins_by_geometric_section": {
                    sign: {
                        section: float(section_summed_values[i, sign_idx, section_idx, 0]
                                       * YR_S / MSUN_G)
                        for section_idx, section in enumerate(GEOMETRIC_SECTIONS)
                    }
                    for sign_idx, sign in enumerate(SIGN_BINS)
                },
            }
            for i in range(len(heights))
        ],
    }
    return flux_rows, derived


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Integrate fluxes through origin-centered, z-aligned cylinders "
                    "in an AMReX plotfile."
    )
    p.add_argument("plotfile")
    p.add_argument("--radius", type=float, help="Cylinder radius in plotfile coordinate units.")
    p.add_argument("--radius-kpc", type=float, help="Cylinder radius in kpc; converted to cm.")
    p.add_argument("--rmin-kpc", "--rmin_kpc", dest="rmin_kpc", type=float)
    p.add_argument("--rmax-kpc", "--rmax_kpc", dest="rmax_kpc", type=float)
    p.add_argument("--height", type=float, help="Half-height in plotfile coordinate units.")
    p.add_argument("--height-kpc", type=float, help="Half-height in kpc; converted to cm.")
    p.add_argument("--zmin-kpc", "--zmin_kpc", dest="zmin_kpc", type=float)
    p.add_argument("--zmax-kpc", "--zmax_kpc", dest="zmax_kpc", type=float)
    p.add_argument(
        "--nbins", type=int, default=64,
        help="Number of log-spaced values in the requested radius or half-height range.",
    )
    for role in FIELD_CANDIDATES:
        p.add_argument(f"--{role}")
    p.add_argument("--temperature")
    p.add_argument("--temperature-bins", nargs="+")
    p.add_argument("--gamma", type=float, default=5.0 / 3.0)
    p.add_argument("--output-json")
    p.add_argument("--list-fields", action="store_true")
    p.add_argument("--progress", action="store_true")
    p.add_argument(
        "--cache-mb", type=int, default=None,
        help="Block-cache budget in MiB (default: AMREXPLORER_CACHE_SIZE_MB or 1 GiB).",
    )
    a = p.parse_args(argv)

    cache = None if a.cache_mb is None else a.cache_mb * 1024 * 1024
    with amrexplorer.open_plotfile(a.plotfile, cache_bytes=cache) as ds:
        available = list(ds.fields)
        if a.list_fields:
            for idx, name in enumerate(available):
                print(f"{idx:03d} {name}")
            return 0

        pc_cm = PC_CM
        kpc_cm = 1.0e3 * pc_cm
        radii = _scan(a.radius, a.radius_kpc, a.rmin_kpc, a.rmax_kpc, a.nbins,
                      pc_cm=pc_cm, name="radius",
                      single_flags=("--radius", "--radius-kpc"),
                      range_flags=("--rmin-kpc", "--rmax-kpc"),
                      finite_values=_finite_radii)
        heights = _scan(a.height, a.height_kpc, a.zmin_kpc, a.zmax_kpc, a.nbins,
                        pc_cm=pc_cm, name="height",
                        single_flags=("--height", "--height-kpc"),
                        range_flags=("--zmin-kpc", "--zmax-kpc"),
                        finite_values=_finite_heights)
        if len(radii) > 1 and len(heights) > 1:
            raise RuntimeError(
                "Radius and half-height cannot both vary in one run; use a radius "
                "range with --height/--height-kpc, or a single radius with a height range"
            )
        temperature_bins = (_parse_temperature_bins(a.temperature_bins)
                            if a.temperature_bins is not None else None)

        fields = {role: _pick_field(role, _parse_field_arg(getattr(a, role)), available)
                  for role in FIELD_CANDIDATES}
        if temperature_bins is not None:
            fields["temperature"] = _pick_field(
                "temperature", _parse_field_arg(a.temperature), available,
                candidates=TEMPERATURE_CANDIDATES)
        print("cylindrical flux surface fields: "
              + ", ".join(f"{role}={name}" for role, name in fields.items()),
              file=sys.stderr, flush=True)

        values = cylindrical_flux_surface_integral(
            ds, fields, radii, heights, gamma=float(a.gamma),
            temperature_bins=temperature_bins, progress=bool(a.progress))

        if len(radii) > 1:
            rows_and_derived = [
                _flux_rows_and_derived(heights, values[radius_idx:radius_idx + 1],
                                       pc_cm=pc_cm, temperature_bins=temperature_bins)
                for radius_idx in range(len(radii))
            ]
        else:
            rows_and_derived = [_flux_rows_and_derived(
                heights, values, pc_cm=pc_cm, temperature_bins=temperature_bins)]

        flux_rows, first_derived = rows_and_derived[0]
        flux_rows_by_radius = []
        derived_by_radius = {
            "mass_flux_msun_per_yr_by_radius": [],
            "radial_angular_momentum_flux_by_radius": [],
            "vertical_angular_momentum_flux_by_radius": [],
        }
        if len(heights) == 1:
            for radius, (radius_flux_rows, radius_derived) in zip(radii, rows_and_derived):
                row = dict(radius_flux_rows[0])
                row.update(radius=float(radius), radius_kpc=float(radius / kpc_cm))
                flux_rows_by_radius.append(row)
                for height_key, radius_key in (
                    ("mass_flux_msun_per_yr_by_height", "mass_flux_msun_per_yr_by_radius"),
                    ("radial_angular_momentum_flux_by_height",
                     "radial_angular_momentum_flux_by_radius"),
                    ("vertical_angular_momentum_flux_by_height",
                     "vertical_angular_momentum_flux_by_radius"),
                ):
                    derived_row = dict(radius_derived[height_key][0])
                    derived_row.update(radius=float(radius), radius_kpc=float(radius / kpc_cm))
                    derived_by_radius[radius_key].append(derived_row)
            if len(radii) > 1:
                derived = derived_by_radius
            else:
                derived = dict(first_derived)
                derived.update(derived_by_radius)
        else:
            derived = first_derived

        single = len(radii) == 1 and len(heights) == 1
        result = {
            "plotfile": a.plotfile,
            "time": float(ds.time),
            "radius": float(radii[0]) if len(radii) == 1 else None,
            "radius_kpc": float(radii[0] / kpc_cm) if len(radii) == 1 else None,
            "radii": [float(radius) for radius in radii],
            "radii_kpc": [float(radius / kpc_cm) for radius in radii],
            "height": float(heights[0]) if len(heights) == 1 else None,
            "height_kpc": float(heights[0] / kpc_cm) if len(heights) == 1 else None,
            "heights": [float(height) for height in heights],
            "heights_kpc": [float(height / kpc_cm) for height in heights],
            "nbins": int(max(len(radii), len(heights))),
            "num_radii": int(len(radii)),
            "num_heights": int(len(heights)),
            "temperature_bins": ([float(edge) for edge in temperature_bins]
                                 if temperature_bins is not None else None),
            "gamma": float(a.gamma),
            "radial_angular_momentum_axis": "z",
            "vertical_angular_momentum_axis": "z",
            "endcap_normal_convention": "upper +z, lower -z; both summed",
            "center": [0.0, 0.0, 0.0],
            "magnetic_normalization": "magnetic_energy_density = B^2/2",
            "radial_angular_momentum_flux_units": "g cm^2 s^-2 (for cgs input)",
            "vertical_angular_momentum_flux_units": "g cm^2 s^-2 (for cgs input)",
            "sign_bin_convention": "sign of each flux component, not velocity",
            "components": list(COMPONENTS),
            "fields": dict(fields),
            "fluxes": flux_rows[0]["fluxes"] if single else None,
            "flux_bins": flux_rows[0]["flux_bins"] if single else None,
            "flux_bins_by_geometric_section": (
                flux_rows[0]["flux_bins_by_geometric_section"] if single else None),
            "flux_bins_by_temperature": (
                flux_rows[0]["flux_bins_by_temperature"]
                if single and temperature_bins is not None else None),
            "fluxes_by_height": flux_rows if len(radii) == 1 else None,
            "fluxes_by_radius": flux_rows_by_radius if len(heights) == 1 else None,
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
