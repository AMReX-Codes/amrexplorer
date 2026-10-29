#!/usr/bin/env python3
"""Write a synthetic 3-D MHD plotfile for comparing the flux-surface ports
with Kangaroo: Quokka's field names (gasDensity, x/y/z-GasMomentum,
gasEnergy, gasInternalEnergy, scalar_0, x/y/z-BField, temperature, gpot),
three levels with partial
refinement about the origin, float64 throughout and no stored ghost cells
(Kangaroo's reader does not skip them). The flow has outflow, rotation and
infall so every sign bin and angular-momentum term is populated."""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..",
                                "tests", "python"))
from plotfile_writer import Level, write_plotfile  # noqa: E402

KPC = 3.0856775814913673e21
HALF_WIDTH = 8 * KPC
GAMMA = 5.0 / 3.0


def _position(level, i, j, k):
    dx = 2 * HALF_WIDTH / (16 * 2**level)
    return tuple(-HALF_WIDTH + (n + 0.5) * dx for n in (i, j, k))


def _state(level, i, j, k):
    x, y, z = _position(level, i, j, k)
    r = np.sqrt(x * x + y * y + z * z) + 1e-3 * KPC
    R = np.hypot(x, y) + 1e-3 * KPC
    rho = 1e-24 * np.exp(-R / (3 * KPC) - np.abs(z) / KPC) * (
        1 + 0.3 * np.sin(i + 2 * j - k))
    vr = 1e7 * np.tanh((r - 4 * KPC) / KPC) * (1 + 0.5 * np.cos(i - j))
    vphi = 2e7 * np.exp(-R / (5 * KPC))
    vx = vr * x / r - vphi * y / R
    vy = vr * y / r + vphi * x / R
    vz = vr * z / r + 3e6 * np.sin(j + k)
    bx = 1e-6 * (1 + 0.2 * np.sin(k))
    by = 2e-6 * np.cos(i + 0.5 * j)
    bz = 5e-7 * np.sin(i - k)
    temperature = 10 ** (2 + 5 * np.abs(np.sin(0.3 * i + 0.2 * j + 0.1 * k)))
    pressure = rho * 1.380649e-16 * temperature / (0.6 * 1.6726e-24)
    energy = (pressure / (GAMMA - 1) + 0.5 * rho * (vx * vx + vy * vy + vz * vz)
              + 0.5 * (bx * bx + by * by + bz * bz))
    # A harmonic potential: radial gravity Omega^2 R, so v_c = Omega R and
    # kappa = 2 Omega; its central-difference gradient is exact on a uniform
    # level.
    omega = 2e7 / (5 * KPC)
    potential = 0.5 * omega**2 * (x * x + y * y) + 0.5 * (3 * omega)**2 * z * z
    return dict(rho=rho, vx=vx, vy=vy, vz=vz, bx=bx, by=by, bz=bz,
                energy=energy, temperature=temperature,
                internal=pressure / (GAMMA - 1), potential=potential,
                scalar=rho * (0.5 + 0.5 * np.cos(0.4 * i)))


def field(name):
    def evaluate(level, i, j, k):
        s = _state(level, i, j, k)
        return {
            "gasDensity": s["rho"],
            "x-GasMomentum": s["rho"] * s["vx"],
            "y-GasMomentum": s["rho"] * s["vy"],
            "z-GasMomentum": s["rho"] * s["vz"],
            "gasEnergy": s["energy"],
            "scalar_0": s["scalar"],
            "x-BField": np.broadcast_to(s["bx"], np.shape(i)),
            "y-BField": s["by"],
            "z-BField": s["bz"],
            "temperature": s["temperature"],
            "gasInternalEnergy": s["internal"],
            "gpot": s["potential"],
        }[name]
    return evaluate


NAMES = ["gasDensity", "x-GasMomentum", "y-GasMomentum", "z-GasMomentum",
         "gasEnergy", "scalar_0", "x-BField", "y-BField", "z-BField",
         "temperature", "gasInternalEnergy", "gpot"]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output")
    parser.add_argument("--levels", type=int, default=3, choices=(1, 2, 3),
                        help="refinement levels to keep (1: one uniform level)")
    parser.add_argument("--single-grid", action="store_true",
                        help="level 0 as one grid (implies --levels 1)")
    args = parser.parse_args()
    levels = [
        Level([((0, 0, 0), (7, 15, 15)), ((8, 0, 0), (15, 7, 15)),
               ((8, 8, 0), (15, 15, 15))]),
        Level([((8, 8, 8), (23, 23, 23)), ((4, 12, 12), (7, 19, 19))]),
        Level([((24, 24, 24), (39, 39, 39))]),
    ]
    if args.single_grid:
        levels = [Level([((0, 0, 0), (15, 15, 15))])]
    write_plotfile(args.output, dim=3, domain_cells=(16, 16, 16),
                   fields={name: field(name) for name in NAMES},
                   levels=levels[: args.levels],
                   prob_lo=(-HALF_WIDTH,) * 3, prob_hi=(HALF_WIDTH,) * 3,
                   time=3.0e15, quokka_metadata=True)


if __name__ == "__main__":
    main()
