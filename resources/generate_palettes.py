#!/usr/bin/env python3
"""Generate AMReXplorer's built-in palettes.

Outputs two artifacts:

  * ``palettes/<name>.pal`` -- legacy sequential palette files (256 red bytes,
    then 256 green, then 256 blue, then a 256-byte alpha ramp set to 255, so
    1024 bytes total). The legacy ``Palette::load`` parses this planar layout.
  * ``src/render2d/BuiltinPalettes.cpp`` -- the same tables compiled in as
    ``std::array<Palette::Rgb, 256>``, plus ``builtinPalette()`` and
    ``builtinPaletteName()`` (the menu label, returned *without* a ``.pal``
    extension).

The curated set draws on the popular visualization packages' default and
perceptually-uniform colormaps:

  * ``rainbow``  -- the legacy Amrvis default, read back from
    ``palettes/rainbow.pal`` unchanged (byte-exact).
  * ``turbo``, ``viridis``, ``plasma``, ``coolwarm`` -- sampled from the
    installed matplotlib colormaps (matplotlib is the colormap source shared
    by yt and by ParaView/VisIt's matplotlib-compatible tables).
  * ``parula``   -- MATLAB's default, built from the published BIDS/colormap
    control points (see PARULA_CONTROL_POINTS below).
  * ``blackbody`` -- a black-body radiation thermal ramp (black -> red ->
    orange -> yellow -> white), computed from the Planck-locus chromaticity;
    the signature ParaView/VTK non-diverging preset.

Run from the repository root::

    python3 resources/generate_palettes.py

Requires matplotlib >= 3.3 (turbo landed in 3.3; viridis and plasma in 3.0).
No network access is needed -- the parula control points are embedded.
"""

from __future__ import annotations

import hashlib
import math
from pathlib import Path

import numpy as np
from matplotlib import colormaps
from matplotlib.colors import LinearSegmentedColormap
import matplotlib

REPO_ROOT = Path(__file__).resolve().parent.parent
PALETTE_DIR = REPO_ROOT / "palettes"
BUILTIN_CPP = REPO_ROOT / "src" / "render2d" / "BuiltinPalettes.cpp"

SLOTS = 256

# MATLAB parula control points, transcribed verbatim from
# https://github.com/BIDS/colormap/blob/master/parula.py (a copy of MATLAB's
# parula; The MathWorks holds the rights to that colormap). LinearSegmentedColormap
# interpolates these ~64 RGB stops into a smooth 256-entry table.
PARULA_CONTROL_POINTS = [
    [0.2081, 0.1663, 0.5292], [0.2116238095, 0.1897809524, 0.5776761905],
    [0.212252381, 0.2137714286, 0.6269714286], [0.2081, 0.2386, 0.6770857143],
    [0.1959047619, 0.2644571429, 0.7279], [0.1707285714, 0.2919380952, 0.779247619],
    [0.1252714286, 0.3242428571, 0.8302714286], [0.0591333333, 0.3598333333, 0.8683333333],
    [0.0116952381, 0.3875095238, 0.8819571429], [0.0059571429, 0.4086142857, 0.8828428571],
    [0.0165142857, 0.4266, 0.8786333333], [0.032852381, 0.4430428571, 0.8719571429],
    [0.0498142857, 0.4585714286, 0.8640571429], [0.0629333333, 0.4736904762, 0.8554380952],
    [0.0722666667, 0.4886666667, 0.8467], [0.0779428571, 0.5039857143, 0.8383714286],
    [0.079347619, 0.5200238095, 0.8311809524], [0.0749428571, 0.5375428571, 0.8262714286],
    [0.0640571429, 0.5569857143, 0.8239571429], [0.0487714286, 0.5772238095, 0.8228285714],
    [0.0343428571, 0.5965809524, 0.819852381], [0.0265, 0.6137, 0.8135],
    [0.0238904762, 0.6286619048, 0.8037619048], [0.0230904762, 0.6417857143, 0.7912666667],
    [0.0227714286, 0.6534857143, 0.7767571429], [0.0266619048, 0.6641952381, 0.7607190476],
    [0.0383714286, 0.6742714286, 0.743552381], [0.0589714286, 0.6837571429, 0.7253857143],
    [0.0843, 0.6928333333, 0.7061666667], [0.1132952381, 0.7015, 0.6858571429],
    [0.1452714286, 0.7097571429, 0.6646285714], [0.1801333333, 0.7176571429, 0.6424333333],
    [0.2178285714, 0.7250428571, 0.6192619048], [0.2586428571, 0.7317142857, 0.5954285714],
    [0.3021714286, 0.7376047619, 0.5711857143], [0.3481666667, 0.7424333333, 0.5472666667],
    [0.3952571429, 0.7459, 0.5244428571], [0.4420095238, 0.7480809524, 0.5033142857],
    [0.4871238095, 0.7490619048, 0.4839761905], [0.5300285714, 0.7491142857, 0.4661142857],
    [0.5708571429, 0.7485190476, 0.4493904762], [0.609852381, 0.7473142857, 0.4336857143],
    [0.6473, 0.7456, 0.4188], [0.6834190476, 0.7434761905, 0.4044333333],
    [0.7184095238, 0.7411333333, 0.3904761905], [0.7524857143, 0.7384, 0.3768142857],
    [0.7858428571, 0.7355666667, 0.3632714286], [0.8185047619, 0.7327333333, 0.3497904762],
    [0.8506571429, 0.7299, 0.3360285714], [0.8824333333, 0.7274333333, 0.3217],
    [0.9139333333, 0.7257857143, 0.3062761905], [0.9449571429, 0.7261142857, 0.2886428571],
    [0.9738952381, 0.7313952381, 0.266647619], [0.9937714286, 0.7454571429, 0.240347619],
    [0.9990428571, 0.7653142857, 0.2164142857], [0.9955333333, 0.7860571429, 0.196652381],
    [0.988, 0.8066, 0.1793666667], [0.9788571429, 0.8271428571, 0.1633142857],
    [0.9697, 0.8481380952, 0.147452381], [0.9625857143, 0.8705142857, 0.1309],
    [0.9588714286, 0.8949, 0.1132428571], [0.9598238095, 0.9218333333, 0.0948380952],
    [0.9661, 0.9514428571, 0.0755333333], [0.9763, 0.9831, 0.0538],
]


def sample_colormap(cmap, slots=SLOTS):
    """Sample a matplotlib Colormap at ``slots`` evenly spaced points in [0, 1]."""
    rgba = cmap(np.linspace(0.0, 1.0, slots))
    rgb = np.clip(rgba[:, :3] * 255.0 + 0.5, 0, 255).astype(np.uint8)
    return rgb


def read_planar_pal(path):
    """Read a legacy planar .pal (256R + 256G + 256B [+ 256A]) into a 256x3 array."""
    raw = path.read_bytes()
    if len(raw) not in (3 * SLOTS, 4 * SLOTS):
        raise ValueError(f"{path}: unexpected palette size {len(raw)} bytes")
    arr = np.frombuffer(raw[:3 * SLOTS], dtype=np.uint8).reshape(3, SLOTS)
    return np.ascontiguousarray(arr.T)  # (256, 3): slot-major


def write_planar_pal(path, rgb):
    """Write a 256x3 RGB array as a 1024-byte planar .pal (with opaque alpha)."""
    assert rgb.shape == (SLOTS, 3), rgb.shape
    flat = np.ascontiguousarray(rgb.T).reshape(-1)  # R0..R255 G0..G255 B0..B255
    alpha = np.full(SLOTS, 255, dtype=np.uint8)
    path.write_bytes(flat.tobytes() + alpha.tobytes())


def sha256_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _smoothstep(edge0, edge1, x):
    t = max(0.0, min(1.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


def _blackbody_rgb255(temperature):
    """sRGB color of a blackbody radiator at ``temperature`` (Kelvin).

    Uses the Hellard (2012) analytic fit to the CIE chromaticity of a Planck
    radiator -- the same Planck-locus data that the Plotly/ParaView "Black-Body
    Radiation" tables are built from. Valid for ~1000-40000 K; returns the
    *chromaticity* at unit brightness as (r, g, b) in [0, 255].
    """
    c = temperature / 100.0
    if temperature <= 6600:
        red = 255.0
    else:
        red = 329.698727446 * (c ** -0.1332047592)
    if temperature <= 6600:
        green = 99.4708025861 * math.log(c) - 161.1195681661
    else:
        green = 288.1221695283 * (c ** -0.0755148492)
    if temperature >= 6600:
        blue = 255.0
    elif temperature <= 1900:
        blue = 0.0
    else:
        blue = 138.5177312231 * math.log(c - 10) - 305.0447927307
    return (max(0.0, min(255.0, red)),
            max(0.0, min(255.0, green)),
            max(0.0, min(255.0, blue)))


def blackbody_colormap(slots=SLOTS):
    """A 256-slot black-body radiation ramp: black -> deep red -> orange ->
    yellow -> white.

    The hue at each slot is the blackbody chromaticity (see
    ``_blackbody_rgb255``) at a temperature log-spaced from 1000 K (slot 0) to
    6500 K (the last slot, ~D65 white). Brightness is compressed into the
    bottom quarter via a smoothstep so the ramp starts at pure black and runs
    through saturated reds/oranges rather than beginning at a bright red.
    """
    bright_ramp = 0.25
    rgb = np.zeros((slots, 3), dtype=np.float64)
    for i in range(slots):
        t = i / (slots - 1)
        temperature = 1000.0 * (6.5 ** t)
        r, g, b = _blackbody_rgb255(temperature)
        bright = 1.0 if t >= bright_ramp else _smoothstep(0.0, bright_ramp, t)
        rgb[i] = (r * bright, g * bright, b * bright)
    return np.clip(rgb + 0.5, 0, 255).astype(np.uint8)


def _matplotlib_entry(enum_name, cmap_name):
    """Build a palette entry by sampling an installed matplotlib colormap."""
    cmap = colormaps.get(cmap_name)
    if cmap is None:
        raise SystemExit(f"matplotlib has no colormap named {cmap_name!r}")
    return (enum_name, cmap_name, sample_colormap(cmap),
            f"matplotlib {matplotlib.__version__} colormap {cmap_name!r}")


def build_palettes():
    """Return an ordered list of (enum_name, menu_name, rgb256, source_note).

    Grouped for the menu: spectral (rainbow, turbo), perceptually-uniform
    sequential (viridis, plasma, parula), diverging (coolwarm), thermal
    (blackbody).
    """
    rainbow = read_planar_pal(PALETTE_DIR / "rainbow.pal")
    parula_cmap = LinearSegmentedColormap.from_list("parula", PARULA_CONTROL_POINTS)
    return [
        ("Rainbow", "rainbow", rainbow,
         "palettes/rainbow.pal (legacy Amrvis default, read back unchanged)"),
        _matplotlib_entry("Turbo", "turbo"),
        _matplotlib_entry("Viridis", "viridis"),
        _matplotlib_entry("Plasma", "plasma"),
        ("Parula", "parula", sample_colormap(parula_cmap),
         "parula control points from BIDS/colormap (MATLAB parula)"),
        _matplotlib_entry("Coolwarm", "coolwarm"),
        ("Blackbody", "blackbody", blackbody_colormap(),
         "blackbody radiation: Planck-locus chromaticity (Hellard 2012 fit), "
         "log-spaced 1000-6500 K"),
    ]


def format_array(var_name, rgb, source_note):
    lines = ["// " + source_note]
    # std::array aggregate init needs double outer braces: {{ {...}, {...} }}.
    lines.append(
        "const std::array<Palette::Rgb, Palette::slotCount> "
        + var_name + " = {{")
    body_lines = []
    for slot in rgb:
        r, g, b = int(slot[0]), int(slot[1]), int(slot[2])
        body_lines.append("{" + f"{r:3d},{g:3d},{b:3d}" + "}")
    for i in range(0, len(body_lines), 5):
        chunk = ", ".join(body_lines[i:i + 5])
        sep = "," if i + 5 < len(body_lines) else ""
        lines.append("    " + chunk + sep)
    lines.append("}};")
    return "\n".join(lines)


def write_cpp(entries):
    checksums = "\n".join(
        f"//   {menu}.pal ({note}): {sha256_file(PALETTE_DIR / (menu + '.pal'))}"
        for _, menu, _, note in entries)
    enum_names = [e[0] for e in entries]
    first_var = entries[0][0][0].lower() + entries[0][0][1:]
    default_menu = entries[0][1]

    palette_stats = "\n".join(
        format_array(enum_name[0].lower() + enum_name[1:] + "Slots", rgb, note)
        for enum_name, _, rgb, note in entries)

    statics = "\n".join(
        "    static const Palette {0}({0}Slots);".format(
            enum[0].lower() + enum[1:])
        for enum in enum_names)
    switch_palette = "\n".join(
        "    case BuiltinPalette::{0}: return {1};".format(
            enum, enum[0].lower() + enum[1:])
        for enum in enum_names)
    switch_name = "\n".join(
        '    case BuiltinPalette::{0}: return "{1}";'.format(enum, menu)
        for enum, menu, _, _ in entries)

    # Build the C++ with placeholder tokens + .replace so the many literal
    # braces in C++ never collide with Python format/f-string syntax.
    cpp = """// Generated by resources/generate_palettes.py -- do not edit by hand.
// Regenerate with: python3 resources/generate_palettes.py
//
// Curated built-in palettes drawn from popular visualization packages:
//   rainbow  : the legacy Amrvis default (byte-exact)
//   turbo, viridis, plasma, coolwarm : matplotlib colormaps (also the colormap
//              tables yt and ParaView/VisIt expose)
//   parula   : MATLAB's default, from the published BIDS/colormap control pts
//   blackbody: black-body radiation thermal ramp (Planck-locus chromaticity),
//              the signature ParaView/VTK non-diverging preset
//
// Source checksums (sha256):
@@CHECKSUMS@@
//
// Each table holds the 256 RGB slots in planar palette order. Data values map
// into [Palette::paletteStart, Palette::paletteEnd] = [3, 255]; the first three
// slots are reserved (never addressed by data), matching the legacy layout.

#include <amrexplorer/render2d/Palette.hpp>

#include <array>

namespace amrvis {
namespace {

@@PALETTE_STATS@@

} // namespace

const Palette& builtinPalette(BuiltinPalette palette)
{
@@STATICS@@
    switch (palette) {
@@SWITCH_PALETTE@@
    }
    return @@FIRST_VAR@@;
}

std::string_view builtinPaletteName(BuiltinPalette palette) noexcept
{
    switch (palette) {
@@SWITCH_NAME@@
    }
    return "@@DEFAULT_MENU@@";
}

} // namespace amrvis
"""
    cpp = (cpp
           .replace("@@CHECKSUMS@@", checksums)
           .replace("@@PALETTE_STATS@@", palette_stats)
           .replace("@@STATICS@@", statics)
           .replace("@@SWITCH_PALETTE@@", switch_palette)
           .replace("@@SWITCH_NAME@@", switch_name)
           .replace("@@FIRST_VAR@@", first_var)
           .replace("@@DEFAULT_MENU@@", default_menu))
    BUILTIN_CPP.write_text(cpp)


def main():
    PALETTE_DIR.mkdir(parents=True, exist_ok=True)
    entries = build_palettes()
    # rainbow.pal is read back unchanged and left byte-exact on disk; write the
    # generated palettes (matplotlib + parula) out as .pal files.
    for enum_name, menu, rgb, _ in entries:
        if menu == "rainbow":
            continue
        write_planar_pal(PALETTE_DIR / f"{menu}.pal", rgb)
    write_cpp(entries)
    names = ", ".join(menu for _, menu, _, _ in entries)
    print(f"Wrote {len(entries)} palettes ({names}).")
    print(f"  palette files: {PALETTE_DIR}")
    print(f"  builtin C++:   {BUILTIN_CPP.relative_to(REPO_ROOT)}")


if __name__ == "__main__":
    main()
