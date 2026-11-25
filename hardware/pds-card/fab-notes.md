<!-- SPDX-License-Identifier: MPL-2.0 -->
# Card P: fabrication notes (written by tools/gen_kicad.py)

*2026-11-25, rev P0.1 (draft).* A draft: KiCad's DRC has not been run on it (README).

- **Layers:** 4. **Thickness:** 1.6 mm. **Stackup:** JLCPCB JLC04161H-7628 (or the board house's standard 1.6 mm 4-layer with 7628 prepreg):

| Layer | Use | Thickness (mm) | Material |
|---|---|---|---|
| F.Mask | Top Solder Mask | 0.01 | solder mask |
| F.Cu | L1: signals, components (top) | 0.035 | copper, 1 oz |
| dielectric 1 | prepreg | 0.2104 | 7628, er 4.4 |
| In1.Cu | L2: ground plane (GND), solid | 0.0152 | copper, 0.5 oz |
| dielectric 2 | core | 1.065 | FR4, er 4.6 |
| In2.Cu | L3: +3.3 V plane | 0.0152 | copper, 0.5 oz |
| dielectric 3 | prepreg | 0.2104 | 7628, er 4.4 |
| B.Cu | L4: signals, the slot plug J1 from this side | 0.035 | copper, 1 oz |
| B.Mask | Bottom Solder Mask | 0.01 | solder mask |

- **Finish:** HASL lead-free. Green solder mask both sides, white silkscreen on the top.
- **Rules:** 0.15 mm minimum track and space; vias 0.6 mm pad, 0.3 mm drill, through vias only (no blind or buried vias), no via-in-pad, vias tented. Smallest plated hole 0.3 mm; non-plated holes 2.2, 2.75, 2.85 and 3.86 mm.
- **No impedance control** (a 0.15 mm track on L1 over L2 is about 83 ohm).
- **Outline:** 213.25 x 99.42 mm with the slot plug's tab, five 2.0 mm x 45 degree chamfers (Edge.Cuts).
- **Assembly:** SMD on the top side only (`bom.csv`, "SMD (top)"); by hand: the press-fit 96-pin plug J1 from the bottom, the Pi socket J2, the header J5; the blower M1 (optional) on three M2 screws.
