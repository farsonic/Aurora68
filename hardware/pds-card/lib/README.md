<!-- SPDX-License-Identifier: MPL-2.0 -->
# Card P's KiCad libraries

- `aurora68.kicad_sym`: every symbol the schematic uses, drawn by `tools/gen_kicad.py`. The pin tables of three
  of them come from KiCad's official symbol library (`kicad-pin-tables.csv`: CPLD_Altera:EPM570T100,
  Connector:Raspberry_Pi_2_3, Power_Management:LM66100DCK); the 74LVC16373's and the SN74CBTD3384's are the
  TSSOP pinouts of the TI datasheets; the slot plug's pin names are `../se-pds-pinout.csv`.
- `aurora68.pretty/`: the footprints. Those without the `Aurora68_` prefix are KiCad's own (the libraries'
  master branch, November 2026), rewritten for KiCad 8 by `tools/fetch_kicad_lib.py`, each with its origin in
  its description. `Aurora68_*` are made by `tools/gen_kicad.py`: the slot plug from Apple's Figure 17-4, the
  socket for the inverted Pi from Raspberry Pi's drawing, the holes.

**Licence of the KiCad-derived parts:** KiCad's libraries are CC-BY-SA 4.0 with the KiCad libraries exception
(https://www.kicad.org/libraries/license/): using them in a design puts no obligation on the design; the
library files themselves, and works derived from them as libraries, stay CC-BY-SA 4.0. So the footprints from
KiCad and `kicad-pin-tables.csv` are CC-BY-SA 4.0; the rest of this directory is MPL-2.0 like the project's tools.
