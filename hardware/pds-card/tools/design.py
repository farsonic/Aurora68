# SPDX-License-Identifier: MPL-2.0
"""design.py - Card P as data: every part, every pin's net, every placement, every height.

This is the single source the generators read (gen_kicad.py writes the symbols, footprints, schematic, board,
BOM and the CPLD's pin file from it; validate.py checks the written files against it and against Apple's
numbers). Coordinates are Apple's: Designing Cards and Drivers for the Macintosh Family, 3rd edition (1992),
Figure 17-1, "Component (top) view", in millimetres, origin at the top-left corner of the maximum outline,
x to the right (towards the slot connector and the logic board's right edge), y downwards (towards the front
of the machine). The card's plug sits at the right-hand end, pin 1 towards the front.

Sources of the numbers (the README repeats them with the reasoning):
  - Outline, holes, plug position: Figure 17-1 (213.25 x 120.42, the 110.11 body, the 16.00 rear strip, two
    3.86 holes at x = 3.43, 50.17 apart, the lower one 25.09 above the 110.11 edge; the plug's row B 200.66 to
    the right of the holes and its centre 14.61 above the lower hole; Figure 17-5 gives the same 200.66 and
    14.61 from the logic board's side), Figure 17-4 (the plug: 2.54 mm grid, fixing holes 2.85 mm at 90.0 mm).
  - Height: Figure 17-2, "Side view height restrictions": 15.58 mm at the front, 4.61 mm at the rear (the side
    view's right-hand end, where Figure 17-8 puts the rear housing and the connector card).
  - Pinout: se-pds-pinout.csv (Figure 13-1, Table 13-1).
  - The Pi 3A+: Raspberry Pi's mechanical drawing (65 x 56 mm, holes 3.5 mm in on a 58 x 49 mm grid, the
    header centred at x = 32.5 and 52.5 mm up, header 8.5 mm high, USB 7.0, HDMI 6.5, audio 6.0, camera and
    display connectors 5.5). The 3B+'s drawing: USB 16.0, RJ45 13.5.
  - The CPLD's pins: the PiStorm Rev B's pistorm.qsf for every signal the PiStorm has.
"""

import csv
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# --------------------------------------------------------------------------------------------------------
# Apple's geometry (Figure 17-1, 17-2, 17-4, 17-5)
# --------------------------------------------------------------------------------------------------------
APPLE_W, APPLE_H, APPLE_BODY_H = 213.25, 120.42, 110.11
APPLE_TAB_X = 195.29                  # the plug's tab: x from 195.29 to 213.25 reaches down to 120.42
APPLE_REAR_STRIP = 16.00              # "No components, traces only", along the rear (top) edge
APPLE_CHAMFER = 2.0                   # 2.0 x 45 degrees, "4PL"
APPLE_THICKNESS = 1.60
APPLE_HOLE_D = 3.86
APPLE_HOLES = [(3.43, 85.02 - 50.17), (3.43, APPLE_BODY_H - 25.09)]     # (3.43, 34.85), (3.43, 85.02)
ROW_X = {'C': 3.43 + 200.66 - 2.54, 'B': 3.43 + 200.66, 'A': 3.43 + 200.66 + 2.54}   # row A towards the board edge
PLUG_CENTRE_Y = APPLE_HOLES[1][1] - 14.61                               # 70.41
def plug_pin_xy(row, pin):
    """Pin 1 towards the front (larger y), pin 32 towards the rear, 2.54 mm apart, centred on PLUG_CENTRE_Y."""
    return ROW_X[row], PLUG_CENTRE_Y + (16.5 - pin) * 2.54
PLUG_FIX_HOLES = [(ROW_X['B'] + 0.3, PLUG_CENTRE_Y - 45.0), (ROW_X['B'] + 0.3, PLUG_CENTRE_Y + 45.0)]
PLUG_FIX_D = 2.85
H_FRONT, H_REAR = 15.58, 4.61         # Figure 17-2
H_FRONT_Y, H_REAR_Y = APPLE_BODY_H, 0.0


def height_limit(y):
    """Apple's component height at a point y mm from the rear edge: 4.61 at the rear edge, rising linearly to
    15.58 at the front edge of the card's body (the plug's tab, further forward, is held at 15.58). The side view
    gives the two numbers at the ends of a straight chassis edge; reading them as the card's own rear and front
    edges is our reading (README, "Height")."""
    t = (y - H_REAR_Y) / (H_FRONT_Y - H_REAR_Y)
    t = max(0.0, min(1.0, t))
    return H_REAR + (H_FRONT - H_REAR) * t


# The card's own outline: Apple's maximum, less the rear 21 mm (the 16 mm strip Apple allows no parts on,
# and 5 mm more), which makes the board smaller and cheaper. Everything else as Apple draws it.
CARD_REAR_Y = 21.0
def card_outline():
    """A list of (x, y) corners, clockwise, with 2 mm chamfers at the outer corners."""
    c = APPLE_CHAMFER
    W, B, H, T = APPLE_W, APPLE_BODY_H, APPLE_H, APPLE_TAB_X
    R = CARD_REAR_Y
    return [(0, R + c), (c, R), (W - c, R), (W, R + c), (W, H - c), (W - c, H), (T + c, H), (T, H - c),
            (T, B), (c, B), (0, B - c)]


# --------------------------------------------------------------------------------------------------------
# The Pi 3A+ on the card: components down, on a 2x20 socket. Its own coordinates (u, v) are those of
# Raspberry Pi's drawing: u along the 65 mm edge from the corner away from the USB socket, v up the 56 mm edge
# from the HDMI edge; the header is at v = 52.5. It is turned over about its long axis (u), see pi_xy().
# --------------------------------------------------------------------------------------------------------
PI_W, PI_D = 65.0, 56.0
PI_PCB_T = 1.4                        # Raspberry Pi boards are 1.4 mm (to check against a real board)
PI_HEADER_BODY = 2.5                  # the Pi's own header: 2.5 mm plastic, 6.0 mm pins above it (Z 8.5)
SOCKET_H = 6.0                        # the card's 2x20 socket body (open-bottom, the Pi's pins seated fully)
PI_GAP = SOCKET_H + PI_HEADER_BODY    # card surface to the Pi's component side: 8.5 mm (standoffs 8.5 mm)
PI_TOP = PI_GAP + PI_PCB_T            # card surface to the Pi's back (now facing up): 9.9 mm
PI_X0 = 12.0                          # the Pi's u = 0 edge (its microSD end) lies at x = 12
PI_Y0 = 53.9                          # its v = 0 edge (HDMI, audio) at y = 53.9: as far back as Apple's height allows
def pi_xy(u, v):
    """The Pi turned components-down about its long axis: u still runs to the right (+x), v now runs towards
    the front (+y). So the header (v = 52.5) lies along the Pi's front edge, near the card's front edge, the USB
    socket (u = 65) at its right-hand end facing the bus half, and the HDMI and audio sockets, which stand out
    beyond the board's edge, overhang towards the rear, over the card, rather than beyond its front edge. (Turned
    over about its short axis instead, the header would be at the rear and HDMI and audio would stand out past the
    card's front edge.)"""
    return PI_X0 + u, PI_Y0 + v
def pi_header_pin(n):
    k = (n - 1) // 2
    u = 32.5 - 24.13 + k * 2.54
    v = 52.5 - 1.27 if n % 2 == 1 else 52.5 + 1.27         # odd pins on the inner row, 5 V pins at the edge
    return pi_xy(u, v)
PI_HOLES_3AP = [pi_xy(u, v) for u, v in ((3.5, 3.5), (61.5, 3.5), (3.5, 52.5), (61.5, 52.5))]
PI_HOLES_Z2W = [pi_xy(u, v) for u, v in ((3.5, 29.5), (61.5, 29.5))]      # the Zero 2 W's other two holes
PI_HOLE_D = 2.75
# the Pi's parts that face the card (components down) and their heights above the Pi's board (drawing)
PI_TOP_PARTS = [  # name, u0, u1, v0, v1, height
    ('USB-A', 50.0, 67.0, 24.5, 38.5, 7.0), ('HDMI', 24.5, 39.5, -1.0, 11.5, 6.5),
    ('audio', 50.0, 57.0, -2.0, 12.0, 6.0), ('camera', 42.0, 48.0, 0.0, 22.0, 5.5),
    ('display', 1.0, 6.0, 15.0, 37.0, 5.5), ('header', 7.0, 58.0, 50.0, 55.0, 8.5),
    ('micro-USB', 6.5, 14.5, -1.0, 4.5, 3.0), ('SoC + heatsink', 20.5, 34.0, 24.2, 38.3, 7.4)]
# the Pi's own underside (now facing up): the microSD socket and its card, at the u = 0 end
PI_UNDERSIDE = [('microSD socket and card', -2.5, 15.0, 20.0, 36.0, 1.6)]
# the 3B / 3B+ beyond the 3A+'s 65 mm: their USB pairs and RJ45 at u = 65..87 (drawing: Z 16.0 and 13.5)
PI3B_EXT = [('3B/3B+ USB-A pairs', 65.0, 87.1, 2.5, 37.0, 16.0), ('3B/3B+ RJ45', 65.0, 87.1, 39.0, 55.0, 13.5)]
PI3B_ALLOWANCE = 0.42                 # the owner accepts the 3B's USB stack 0.42 mm over Apple's 15.58 mm

# --------------------------------------------------------------------------------------------------------
# Footprints: name, source, height above the card's top surface (mm), side. Fetched ones come from KiCad's
# official library (tools/fetch_kicad_lib.py notes where); the others are generated in gen_kicad.py.
# --------------------------------------------------------------------------------------------------------
FOOTPRINTS = {
    'TQFP-100_14x14mm_P0.5mm': ('kicad:Package_QFP', 1.6),
    'TSSOP-48_6.1x12.5mm_P0.5mm': ('kicad:Package_SO', 1.2),
    'TSSOP-24_4.4x7.8mm_P0.65mm': ('kicad:Package_SO', 1.2),
    'SOT-223-3_TabPin2': ('kicad:Package_TO_SOT_SMD', 1.8),
    'SOT-363_SC-70-6': ('kicad:Package_TO_SOT_SMD', 1.1),
    'SOT-23': ('kicad:Package_TO_SOT_SMD', 1.25),
    'R_0603_1608Metric': ('kicad:Resistor_SMD', 0.55),
    'C_0603_1608Metric': ('kicad:Capacitor_SMD', 0.9),
    'C_0805_2012Metric': ('kicad:Capacitor_SMD', 1.45),
    'Fuse_1812_4532Metric': ('kicad:Fuse', 1.8),
    'Fuse_1206_3216Metric': ('kicad:Fuse', 1.3),
    'LED_0603_1608Metric': ('kicad:LED_SMD', 0.8),
    'D_SOD-123': ('kicad:Diode_SMD', 1.35),
    'PinHeader_1x02_P2.54mm_Vertical': ('kicad:Connector_PinHeader_2.54mm', 8.5),   # with its shunt
    'PinHeader_1x02_P2.54mm_Horizontal': ('kicad:Connector_PinHeader_2.54mm', 3.8),  # with a cable housing
    'PinHeader_1x03_P2.54mm_Horizontal': ('kicad:Connector_PinHeader_2.54mm', 3.8),
    'PinHeader_1x04_P2.54mm_Horizontal': ('kicad:Connector_PinHeader_2.54mm', 3.8),
    'TE_1-84952-0_1x10-1MP_P1.0mm_Horizontal': ('kicad:Connector_FFC-FPC', 2.6),
    'Aurora68_PiSocket_2x20_Inverted': ('generated', SOCKET_H),
    'Aurora68_DIN41612_C_3x32_Male_Straight_PressFit_Bottom': ('generated', 1.5),   # press-fit tails on top
    'Aurora68_AppleCardSupport_3.86mm': ('generated', 0.0),
    'Aurora68_PiStandoff_M2.5': ('generated', 0.0),
}

# --------------------------------------------------------------------------------------------------------
# Symbols: pin lists (number, name, electrical type). The CPLD's, the Pi's and the LM66100's pin data are
# KiCad's (CPLD_Altera:EPM570T100, Connector:Raspberry_Pi_2_3, Power_Management:LM66100DCK; CC-BY-SA 4.0 with
# the KiCad library exception); the 74LVC16373's and the CBTD3384's are the TI/Nexperia datasheets' TSSOP pinouts.
# --------------------------------------------------------------------------------------------------------
def _pins_16373():
    p = {1: '~{1OE}', 24: '~{2OE}', 25: '2LE', 48: '1LE'}
    q1 = [2, 3, 5, 6, 8, 9, 11, 12]; q2 = [13, 14, 16, 17, 19, 20, 22, 23]
    d2 = [36, 35, 33, 32, 30, 29, 27, 26]; d1 = [47, 46, 44, 43, 41, 40, 38, 37]
    for i in range(8):
        p[q1[i]] = '1Q%d' % (i + 1); p[q2[i]] = '2Q%d' % (i + 1)
        p[d1[i]] = '1D%d' % (i + 1); p[d2[i]] = '2D%d' % (i + 1)
    for n in (4, 10, 15, 21, 28, 34, 39, 45): p[n] = 'GND'
    for n in (7, 18, 31, 42): p[n] = 'VCC'
    def t(nm):
        return 'power_in' if nm in ('GND', 'VCC') else 'tri_state' if 'Q' in nm else 'input'
    return [(str(n), p[n], t(p[n])) for n in range(1, 49)]

def _pins_cbtd3384():
    names = {1: '~{1OE}', 2: '1B1', 3: '1A1', 4: '1A2', 5: '1B2', 6: '1B3', 7: '1A3', 8: '1A4', 9: '1B4', 10: '1B5',
             11: '1A5', 12: 'GND', 13: '~{2OE}', 14: '2A1', 15: '2B1', 16: '2B2', 17: '2A2', 18: '2A3', 19: '2B3',
             20: '2B4', 21: '2A4', 22: '2A5', 23: '2B5', 24: 'VCC'}
    def t(nm):
        return 'power_in' if nm in ('GND', 'VCC') else 'input' if 'OE' in nm else 'passive'
    return [(str(n), names[n], t(names[n])) for n in range(1, 25)]

def _pins_from_kicad(name):
    """(number, name, type) from a fetched KiCad symbol; fetch_kicad_lib.py keeps a copy of the pin table in
    lib/kicad-pin-tables.csv so a fresh checkout regenerates without the network."""
    rows = []
    with open(os.path.join(ROOT, 'lib', 'kicad-pin-tables.csv'), newline='') as f:
        for r in csv.DictReader(f):
            if r['symbol'] == name:
                rows.append((r['number'], r['name'], r['type']))
    if not rows:
        raise SystemExit('no pin table for %s in lib/kicad-pin-tables.csv' % name)
    return rows

def _pins_two(a='1', b='2'):
    return [('1', a, 'passive'), ('2', b, 'passive')]

SYMBOLS = {
    # name: (pins, reference prefix, description, datasheet)
    'EPM240_570_T100': (lambda: [(n, nm, t) for n, nm, t in _pins_from_kicad('EPM570T100')], 'U',
                        'Altera/Intel MAX II CPLD, TQFP-100: EPM240T100C5N, or EPM570T100C5N (pins 37, 39, 88, 90 are '
                        'its GNDINT/VCCINT; an EPM240 leaves them as unused I/O, so the PiStorm-style footprint takes both)',
                        'https://www.intel.com/content/www/us/en/content-details/654441/max-ii-device-handbook.html'),
    '74LVC16373_TSSOP48': (_pins_16373, 'U', '16-bit transparent D-type latch, 3-state, 5 V tolerant (74LVC16373A)',
                           'https://www.ti.com/lit/ds/symlink/sn74lvc16373a.pdf'),
    'SN74CBTD3384_TSSOP24': (_pins_cbtd3384, 'U', '10-bit FET bus switch with level shifting (as the PiStorm Rev B)',
                             'https://www.ti.com/lit/ds/symlink/sn74cbtd3384.pdf'),
    'RPI_GPIO_SOCKET_2x20': (lambda: [(n, nm, 'passive' if nm in ('3V3', '5V', 'GND') else 'bidirectional')
                                      for n, nm, t in _pins_from_kicad('Raspberry_Pi_2_3')], 'J',
                             '2x20 socket for a Raspberry Pi 3A+ / Zero 2 W turned components-down (pin names as on the Pi)', ''),
    'LM66100DCK': (lambda: _pins_from_kicad('LM66100DCK'), 'U', 'Ideal diode, 1.5 A, 5.5 V, reverse-current blocking',
                   'https://www.ti.com/lit/ds/symlink/lm66100.pdf'),
    'AMS1117-3.3': (lambda: [('1', 'GND', 'power_in'), ('2', 'VO', 'power_out'), ('3', 'VI', 'power_in')], 'U',
                    '1 A 3.3 V LDO, SOT-223 (the PiStorm uses an LM1117-3.3 in the same place)', ''),
    'AO3400A': (lambda: [('1', 'G', 'input'), ('2', 'S', 'passive'), ('3', 'D', 'passive')], 'Q',
                'N-channel MOSFET 30 V 5.7 A, SOT-23 (G S D)', ''),
    'R': (_pins_two, 'R', 'Resistor', ''),
    'C': (_pins_two, 'C', 'Capacitor', ''),
    'Polyfuse': (_pins_two, 'F', 'Resettable fuse (PTC)', ''),
    'LED': (lambda: [('1', 'K', 'passive'), ('2', 'A', 'passive')], 'D', 'LED', ''),
    'D_Schottky': (lambda: [('1', 'K', 'passive'), ('2', 'A', 'passive')], 'D', 'Schottky diode', ''),
    'Conn_01x02': (lambda: [('1', 'Pin_1', 'passive'), ('2', 'Pin_2', 'passive')], 'J', 'Header 1x02', ''),
    'Conn_01x03': (lambda: [(str(i), 'Pin_%d' % i, 'passive') for i in (1, 2, 3)], 'J', 'Header 1x03', ''),
    'Conn_01x04': (lambda: [(str(i), 'Pin_%d' % i, 'passive') for i in (1, 2, 3, 4)], 'J', 'Header 1x04', ''),
    'FFC_01x10_MP': (lambda: [(str(i), 'Pin_%d' % i, 'passive') for i in range(1, 11)] + [('MP', 'MP', 'passive')],
                     'J', 'FFC/FPC connector, 10 positions, 1.0 mm, with mounting pads', ''),
    'SE_PDS_96': (None, 'J', "The Macintosh SE's 96-pin processor direct slot, the card's plug (pin names from se-pds-pinout.csv)", ''),
    'MountingHole': (lambda: [], 'H', 'Mounting hole, no connection', ''),
    'PWR_FLAG': (lambda: [('1', '~', 'power_out')], '#FLG', 'Marks a net as driven, for ERC', ''),
}


def slot_pins():
    rows = []
    with open(os.path.join(ROOT, 'se-pds-pinout.csv'), newline='') as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def slot_net(signal):
    """The card's net name for a slot signal: SE_ and the signal with / turned into a trailing _n."""
    s = signal.strip()
    if s in ('GND',): return 'GND'
    if s == '+5V': return '+5V_SLOT'
    if s == '+12V': return '+12V_SLOT'
    if s == '-5V': return '-5V_SLOT'
    if s == '-12V': return '-12V_SLOT'
    if s in ('Reserved', 'Spare'): return None
    n = s.replace('R/W', 'RW').replace('.', '')
    if n.startswith('/'):
        n = n[1:] + '_n'
    return 'SE_' + n


# --------------------------------------------------------------------------------------------------------
# The CPLD's pins: the PiStorm Rev B's for every signal it has (reference/pistorm/rtl/pistorm.qsf), the new
# ones on pins the PiStorm leaves free. Port names are those of rtl/aurora68_pds.v.
# --------------------------------------------------------------------------------------------------------
CPLD_PINS = {
    'PI_CLK': 12, 'PI_RESET': 34, 'PI_A[1]': 28, 'PI_A[0]': 27,
    'PI_D[0]': 2, 'PI_D[1]': 3, 'PI_D[2]': 4, 'PI_D[3]': 5, 'PI_D[4]': 6, 'PI_D[5]': 7, 'PI_D[6]': 26,
    'PI_D[7]': 21, 'PI_D[8]': 17, 'PI_D[9]': 16, 'PI_D[10]': 20, 'PI_D[11]': 8, 'PI_D[12]': 15, 'PI_D[13]': 14,
    'PI_D[14]': 19, 'PI_D[15]': 18, 'PI_TXN_IN_PROGRESS': 30, 'PI_IPL_ZERO': 33, 'PI_RD': 35, 'PI_WR': 29,
    'LTCH_A_0': 44, 'LTCH_A_8': 42, 'LTCH_A_16': 47, 'LTCH_A_24': 48, 'LTCH_A_OE_n': 43, 'LTCH_D_RD_U': 50,
    'LTCH_D_RD_L': 49, 'LTCH_D_RD_OE_n': 38, 'LTCH_D_WR_U': 36, 'LTCH_D_WR_L': 40, 'LTCH_D_WR_OE_n': 41,
    'M68K_CLK': 62, 'M68K_VMA_n': 52, 'M68K_E': 53, 'M68K_VPA_n': 54, 'M68K_BERR_n': 55, 'M68K_IPL_n[0]': 56,
    'M68K_FC[2]': 57, 'M68K_IPL_n[1]': 58, 'M68K_IPL_n[2]': 61, 'M68K_FC[1]': 64, 'M68K_FC[0]': 66,
    'M68K_RESET_n': 67, 'M68K_PMCYC_n': 68, 'M68K_BR_n': 69, 'M68K_BGACK_n': 70, 'M68K_BG_n': 71,
    'M68K_AS_n': 72, 'M68K_UDS_n': 73, 'M68K_LDS_n': 74, 'M68K_RW': 75, 'M68K_DTACK_n': 76,
    # new on Card P (PiStorm: free pins)
    'SLOT_PWR': 77, 'FAN_PWM': 78, 'FAN_TACH': 81, 'STOCK_SW_n': 82, 'LED_ACT': 83,
}
PISTORM_PIN_NOTES = {68: "the PiStorm's M68K_HALT_n pin (on the SE /HALT is /RESET)", 53: "the PiStorm's E pin (an output there)",
                     69: "the PiStorm's /BR pin (an input there)", 71: "the PiStorm's /BG pin (an output there)",
                     62: "the PiStorm's M68K_CLK pin, through 22 ohm as there"}
CPLD_JTAG = {22: 'JTAG_TMS', 23: 'JTAG_TDI', 24: 'JTAG_TCK', 25: 'JTAG_TDO'}


def port_net(port):
    """The board net of a CPLD port: PI_D[3] -> PI_D3, M68K_FC[2] -> M68K_FC2."""
    return port.replace('[', '').replace(']', '')


# The Pi's header: physical pin -> net (BCM GPIO numbers of emu68/src/pistorm/ps_protocol.h)
GPIO_PIN = {0: 27, 1: 28, 2: 3, 3: 5, 4: 7, 5: 29, 6: 31, 7: 26, 8: 24, 9: 21, 10: 19, 11: 23, 12: 32, 13: 33,
            14: 8, 15: 10, 16: 36, 17: 11, 18: 12, 19: 35, 20: 38, 21: 40, 22: 15, 23: 16, 24: 18, 25: 22,
            26: 37, 27: 13}
GPIO_NET = {0: 'PI_TXN_IN_PROGRESS', 1: 'PI_IPL_ZERO', 2: 'PI_A0', 3: 'PI_A1', 4: 'PI_CLK', 5: 'PI_RESET',
            6: 'PI_RD', 7: 'PI_WR', 24: 'JTAG_TMS', 25: 'JTAG_TDO', 26: 'JTAG_TCK', 27: 'JTAG_TDI'}
for _g in range(8, 24):
    GPIO_NET[_g] = 'PI_D%d' % (_g - 8)


def pi_socket_nets():
    nets = {}
    for g, pin in GPIO_PIN.items():
        nets[str(pin)] = GPIO_NET[g]
    for p in (2, 4): nets[str(p)] = '5V_PI'
    for p in (6, 9, 14, 20, 25, 30, 34, 39): nets[str(p)] = 'GND'
    for p in (1, 17): nets[str(p)] = None            # the Pi's 3.3 V is not used (the card has its own, as the PiStorm)
    return nets


# --------------------------------------------------------------------------------------------------------
# Parts
# --------------------------------------------------------------------------------------------------------
class Part:
    def __init__(self, ref, sym, value, fp, sheet, at, nets, rot=0, side='F', fields=None, height=None, lcsc='',
                 mpn='', note=''):
        self.ref, self.sym, self.value, self.fp, self.sheet = ref, sym, value, fp, sheet
        self.x, self.y = at
        self.rot, self.side = rot, side
        self.nets = nets                # pin number -> net name (None: no connection)
        self.lcsc, self.mpn, self.note = lcsc, mpn, note
        self.height = FOOTPRINTS[fp][1] if height is None else height


def build():
    P = []
    add = P.append

    # ---- J1, the slot plug, on the card's underside ----
    n = {}
    for r in slot_pins():
        n[r['row'] + r['pin']] = slot_net(r['signal'])
    for k in list(n):
        if n[k] in ('+12V_SLOT', '-5V_SLOT', '-12V_SLOT', 'SE_C16M', 'SE_HALT_n', 'SE_EXTDTK_n'):
            n[k] = None                 # not used: no +12/-5/-12 V loads, C16M unused, /HALT is /RESET, /EXT.DTK left alone
    add(Part('J1', 'SE_PDS_96', 'DIN 41612 C 96 male straight press-fit', 'Aurora68_DIN41612_C_3x32_Male_Straight_PressFit_Bottom',
             'slot', (ROW_X['B'], PLUG_CENTRE_Y), n, side='B',
             mpn='DIN 41612 type C, 3x32 (a+b+c), male, straight, press-fit (e.g. ERNI 024393 / Harting 09 03 296 68xx; verify)'))

    # ---- the two CBTD3384 bus switches (A = slot side, B = CPLD side), OE tied low, VCC from 5V_PI ----
    def cbtd(ref, at, chans):
        a = {1: '3', 2: '4', 3: '7', 4: '8', 5: '11', 6: '14', 7: '17', 8: '18', 9: '21', 10: '22'}
        b = {1: '2', 2: '5', 3: '6', 4: '9', 5: '10', 6: '15', 7: '16', 8: '19', 9: '20', 10: '23'}
        nets = {'1': 'GND', '13': 'GND', '12': 'GND', '24': '5V_PI'}
        for i, (se, cp) in enumerate(chans, start=1):
            nets[a[i]] = se; nets[b[i]] = cp
        add(Part(ref, 'SN74CBTD3384_TSSOP24', 'SN74CBTD3384PW', 'TSSOP-24_4.4x7.8mm_P0.65mm', 'slot', at, nets, rot=90,
                 lcsc='C7155', mpn='SN74CBTD3384PWR'))
    # channels grouped by where the signals sit on the plug: U6 the middle (E, interrupts, /BERR at A27 and
    # B18-B21; /RESET to /LDS at C8-C12), U7 the front (R/W to /VPA at C1-C7, the function codes at A1-A3)
    cbtd('U6', (195.0, 72.0), [('SE_E', 'M68K_E'), ('SE_IPL0_n', 'M68K_IPL_n0'), ('SE_IPL1_n', 'M68K_IPL_n1'),
                               ('SE_IPL2_n', 'M68K_IPL_n2'), ('SE_BERR_n', 'M68K_BERR_n'), ('SE_RESET_n', 'M68K_RESET_n'),
                               ('SE_PMCYC_n', 'M68K_PMCYC_n'), ('SE_AS_n', 'M68K_AS_n'), ('SE_UDS_n', 'M68K_UDS_n'),
                               ('SE_LDS_n', 'M68K_LDS_n')])
    cbtd('U7', (195.0, 104.0), [('SE_RW', 'M68K_RW'), ('SE_DTACK_n', 'M68K_DTACK_n'), ('SE_BG_n', 'M68K_BG_n'),
                                ('SE_BGACK_n', 'M68K_BGACK_n'), ('SE_BR_n', 'M68K_BR_n'), ('SE_VMA_n', 'M68K_VMA_n'),
                                ('SE_VPA_n', 'M68K_VPA_n'), ('SE_FC0', 'M68K_FC0'), ('SE_FC1', 'M68K_FC1'),
                                ('SE_FC2', 'M68K_FC2')])
    add(Part('C5', 'C', '100n', 'C_0603_1608Metric', 'slot', (195.0, 77.5), {'1': '5V_PI', '2': 'GND'}, lcsc='C14663'))
    add(Part('C6', 'C', '100n', 'C_0603_1608Metric', 'slot', (195.0, 98.5), {'1': '5V_PI', '2': 'GND'}, lcsc='C14663'))
    add(Part('R3', 'R', '22', 'R_0603_1608Metric', 'slot', (196.5, 45.0), {'1': 'SE_C8M', '2': 'M68K_CLK'}, rot=90,
             lcsc='C23345', note='C8M into the CPLD, as the PiStorm\'s 68000 clock (its R3)'))
    add(Part('R4', 'R', '10k', 'R_0603_1608Metric', 'slot', (176.0, 30.0), {'1': '+5V_SLOT', '2': 'SLOT_PWR'}, lcsc='C25804',
             note='SE power sense: 5 V -> 3.0 V'))
    add(Part('R5', 'R', '15k', 'R_0603_1608Metric', 'slot', (176.0, 32.5), {'1': 'SLOT_PWR', '2': 'GND'}, lcsc='C22809'))

    # ---- the CPLD and the four latches ----
    cp = {}
    for port, pin in CPLD_PINS.items():
        cp[str(pin)] = port_net(port)
    for pin, net in CPLD_JTAG.items():
        cp[str(pin)] = net
    for pin in (9, 31, 45, 59, 80, 94, 13, 39, 63, 88):
        cp[str(pin)] = '+3V3'           # VCCIO and VCCINT: 3.3 V (the PiStorm links VCCINT to 3.3 V through 0 ohm)
    for pin in (10, 32, 46, 60, 79, 93, 11, 37, 65, 90):
        cp[str(pin)] = 'GND'
    for pin in range(1, 101):
        cp.setdefault(str(pin), None)
    add(Part('U1', 'EPM240_570_T100', 'EPM240T100C5N', 'TQFP-100_14x14mm_P0.5mm', 'cpld', (150.0, 72.0), cp,
             mpn='EPM240T100C5N (or EPM570T100C5N, LCSC C40067, if the fitter needs more than 240 LEs)'))
    def latch(ref, at, rot, d1, d2, q1, q2, le1, le2, oe1, oe2):
        nets = {}
        pins = _pins_16373()
        for num, nm, t in pins:
            if nm == 'GND': nets[num] = 'GND'
            elif nm == 'VCC': nets[num] = '+3V3'
            elif nm == '~{1OE}': nets[num] = oe1
            elif nm == '~{2OE}': nets[num] = oe2
            elif nm == '1LE': nets[num] = le1
            elif nm == '2LE': nets[num] = le2
            else:
                grp, kind, i = nm[0], nm[1], int(nm[2]) - 1
                src = {('1', 'D'): d1, ('2', 'D'): d2, ('1', 'Q'): q1, ('2', 'Q'): q2}[(grp, kind)]
                nets[num] = src[i]
        add(Part(ref, '74LVC16373_TSSOP48', '74LVC16373', 'TSSOP-48_6.1x12.5mm_P0.5mm', 'cpld', at, nets, rot=rot,
                 mpn='SN74LVC16373ADGGR'))
    pid = ['PI_D%d' % i for i in range(16)]
    a = ['SE_A%d' % i for i in range(24)]
    d = ['SE_D%d' % i for i in range(16)]
    a[0] = None
    latch('U2', (184.0, 95.0), 180, pid[0:8], pid[8:16], a[0:8], a[8:16], 'LTCH_A_0', 'LTCH_A_8', 'LTCH_A_OE_n', 'LTCH_A_OE_n')
    latch('U3', (184.0, 77.0), 180, pid[0:8], pid[8:16], a[16:24], [None] * 8, 'LTCH_A_16', 'LTCH_A_24', 'LTCH_A_OE_n', 'LTCH_A_OE_n')
    latch('U4', (184.0, 50.0), 180, pid[0:8], pid[8:16], d[0:8], d[8:16], 'LTCH_D_WR_L', 'LTCH_D_WR_U', 'LTCH_D_WR_OE_n', 'LTCH_D_WR_OE_n')
    latch('U5', (172.0, 50.0), 0, d[0:8], d[8:16], pid[0:8], pid[8:16], 'LTCH_D_RD_L', 'LTCH_D_RD_U', 'LTCH_D_RD_OE_n', 'LTCH_D_RD_OE_n')
    for ref, at in (('C1', (178.5, 95.0)), ('C2', (178.5, 77.0)), ('C3', (178.5, 50.0)), ('C4', (166.5, 50.0))):
        add(Part(ref, 'C', '100n', 'C_0603_1608Metric', 'cpld', at, {'1': '+3V3', '2': 'GND'}, rot=90, lcsc='C14663',
                 note='latch decoupling (PiStorm C1-C4)'))
    for ref, at in (('C7', (140.0, 62.0)), ('C8', (140.0, 82.0)), ('C9', (160.0, 62.0)), ('C10', (160.0, 82.0)),
                    ('C11', (145.0, 59.0)), ('C12', (155.0, 85.0))):
        add(Part(ref, 'C', '100n', 'C_0603_1608Metric', 'cpld', at, {'1': '+3V3', '2': 'GND'}, rot=90, lcsc='C14663',
                 note='CPLD VCCIO/VCCINT decoupling (PiStorm C7-C10, C13, C14)'))
    add(Part('R1', 'R', '4.7k', 'R_0603_1608Metric', 'cpld', (137.0, 77.0), {'1': '+3V3', '2': 'JTAG_TMS'}, rot=90, lcsc='C23162',
             note='TMS pull-up (PiStorm R2)'))
    add(Part('R2', 'R', '4.7k', 'R_0603_1608Metric', 'cpld', (137.0, 73.0), {'1': '+3V3', '2': 'JTAG_TDI'}, rot=90, lcsc='C23162',
             note='TDI pull-up (PiStorm R1)'))

    # ---- the Pi ----
    add(Part('J2', 'RPI_GPIO_SOCKET_2x20', 'Pi socket 2x20 6.0 mm', 'Aurora68_PiSocket_2x20_Inverted', 'pi',
             pi_header_pin(1), pi_socket_nets(),
             mpn='2x20 female socket, 2.54 mm, 6.0 mm body, open-bottom contacts accepting 6.0 mm pins (choose; README)'))
    for i, (x, y) in enumerate(PI_HOLES_3AP + PI_HOLES_Z2W, start=3):
        add(Part('H%d' % i, 'MountingHole', 'M2.5 standoff 8.5 mm' if i < 7 else 'M2.5 standoff 8.5 mm (Zero 2 W)',
                 'Aurora68_PiStandoff_M2.5', 'pi', (x, y), {}))

    # ---- power: 5 V from the slot through a polyfuse and an ideal diode to the Pi and the card ----
    add(Part('F1', 'Polyfuse', '1.1A hold', 'Fuse_1812_4532Metric', 'power', (166.0, 106.0), {'1': '+5V_SLOT', '2': '5V_F'},
             mpn='PTC 1812, 1.1 A hold, 2.2 A trip, 6 V or more (e.g. Bourns MF-MSMF110-2)'))
    add(Part('U9', 'LM66100DCK', 'LM66100', 'SOT-363_SC-70-6', 'power', (159.0, 106.0),
             {'1': '5V_F', '2': 'GND', '3': 'GND', '4': None, '5': None, '6': '5V_PI'}, mpn='LM66100DCKR',
             note='ideal diode: a Pi powered from its own USB never feeds the SE'))
    add(Part('C13', 'C', '10u', 'C_0805_2012Metric', 'power', (161.5, 101.5), {'1': '5V_F', '2': 'GND'}, lcsc='C15850'))
    add(Part('C14', 'C', '22u', 'C_0805_2012Metric', 'power', (154.5, 101.5), {'1': '5V_PI', '2': 'GND'}, lcsc='C45783'))
    add(Part('U8', 'AMS1117-3.3', 'AMS1117-3.3', 'SOT-223-3_TabPin2', 'power', (146.0, 104.0),
             {'1': 'GND', '2': '+3V3', '3': '5V_PI'}, lcsc='C6186', note='PiStorm U8 (LM1117-3.3)'))
    add(Part('C15', 'C', '22u', 'C_0805_2012Metric', 'power', (139.0, 100.0), {'1': '5V_PI', '2': 'GND'}, rot=90, lcsc='C45783',
             note='PiStorm C11'))
    add(Part('C16', 'C', '22u', 'C_0805_2012Metric', 'power', (139.0, 107.0), {'1': '+3V3', '2': 'GND'}, rot=90, lcsc='C45783',
             note='PiStorm C12'))
    add(Part('F2', 'Polyfuse', '0.35A hold', 'Fuse_1206_3216Metric', 'power', (166.0, 101.0), {'1': '+5V_SLOT', '2': '5V_FAN'},
             mpn='PTC 1206, 0.35 A hold, 6 V or more'))
    add(Part('F3', 'Polyfuse', '0.75A hold', 'Fuse_1812_4532Metric', 'power', (166.0, 96.0), {'1': '+5V_SLOT', '2': '5V_PANEL'},
             mpn='PTC 1812, 0.75 A hold (Apple allows 750 mA at +5 V for a peripheral outside the case)'))

    # ---- fan, LED, stock switch, USB in, the cable to the rear connector card ----
    add(Part('J3', 'Conn_01x03', 'Fan', 'PinHeader_1x03_P2.54mm_Horizontal', 'panel', (112.0, 106.0),
             {'1': 'FAN_GND_SW', '2': '5V_FAN', '3': 'FAN_TACH'}, rot=90, note='1 GND (switched), 2 +5 V, 3 tachometer'))
    add(Part('Q1', 'AO3400A', 'AO3400A', 'SOT-23', 'panel', (119.0, 99.0), {'1': 'FAN_G', '2': 'GND', '3': 'FAN_GND_SW'},
             lcsc='C20917'))
    add(Part('R8', 'R', '100', 'R_0603_1608Metric', 'panel', (123.0, 96.5), {'1': 'FAN_PWM', '2': 'FAN_G'}, lcsc='C22775'))
    add(Part('R9', 'R', '100k', 'R_0603_1608Metric', 'panel', (123.0, 101.5), {'1': 'FAN_G', '2': 'GND'}, lcsc='C25803'))
    add(Part('D1', 'D_Schottky', 'B5819W', 'D_SOD-123', 'panel', (113.0, 92.0), {'1': '5V_FAN', '2': 'FAN_GND_SW'},
             lcsc='C8598', note='fan flyback'))
    add(Part('JP1', 'Conn_01x02', 'Fan full speed', 'PinHeader_1x02_P2.54mm_Vertical', 'panel', (107.0, 92.0),
             {'1': 'FAN_GND_SW', '2': 'GND'}, rot=90, note='shunt fitted: fan always on, no software needed'))
    add(Part('R6', 'R', '10k', 'R_0603_1608Metric', 'panel', (119.0, 90.0), {'1': '+3V3', '2': 'FAN_TACH'}, lcsc='C25804'))
    add(Part('J6', 'Conn_01x02', 'Stock mode', 'PinHeader_1x02_P2.54mm_Horizontal', 'panel', (127.0, 107.0),
             {'1': 'STOCK_SW_n', '2': 'GND'}, rot=90, note='closed: stock SE (jumper or a switch on a cable)'))
    add(Part('R7', 'R', '10k', 'R_0603_1608Metric', 'panel', (129.0, 96.5), {'1': '+3V3', '2': 'STOCK_SW_n'}, lcsc='C25804'))
    add(Part('D2', 'LED', 'green', 'LED_0603_1608Metric', 'panel', (133.0, 108.0), {'1': 'GND', '2': 'LED_A'}, rot=180,
             note='activity'))
    add(Part('R10', 'R', '330', 'R_0603_1608Metric', 'panel', (133.0, 103.5), {'1': 'LED_ACT', '2': 'LED_A'}, lcsc='C23138'))
    add(Part('R11', 'R', '330', 'R_0603_1608Metric', 'panel', (130.0, 30.0), {'1': 'LED_ACT', '2': 'LED_PANEL'}, lcsc='C23138',
             note='the connector card\'s LED, over the cable'))
    add(Part('J5', 'Conn_01x04', 'USB from Pi', 'PinHeader_1x04_P2.54mm_Horizontal', 'panel', (108.0, 82.0),
             {'1': None, '2': 'USB_DM', '3': 'USB_DP', '4': 'GND'}, rot=90,
             note='a short USB-A plug pigtail from the Pi 3A+\'s USB socket, which faces it; pin 1 (VBUS) deliberately not connected'))
    add(Part('J4', 'FFC_01x10_MP', 'Rear connector card', 'TE_1-84952-0_1x10-1MP_P1.0mm_Horizontal', 'panel', (118.0, 26.5),
             {'1': 'GND', '2': 'USB_DM', '3': 'USB_DP', '4': 'GND', '5': '5V_PANEL', '6': '5V_PANEL', '7': 'GND',
              '8': 'STOCK_SW_n', '9': 'LED_PANEL', '10': 'GND', 'MP': 'GND'}, rot=180,
             mpn='TE 1-84952-0 (FFC 1.0 mm, 10 positions, ZIF)'))
    # ---- Apple's card supports ----
    for i, (x, y) in enumerate(APPLE_HOLES, start=1):
        add(Part('H%d' % i, 'MountingHole', 'Apple card support 3.86 mm', 'Aurora68_AppleCardSupport_3.86mm', 'panel', (x, y), {}))
    return P


SHEETS = [  # file, title, what is on it
    ('slot', 'Slot plug and bus switches', 'J1, the CBTD3384 switches, C8M, the SE power sense'),
    ('cpld', 'CPLD and latches', 'the EPM240, the four 74LVC16373, JTAG pull-ups, decoupling'),
    ('pi', 'Raspberry Pi socket', 'the 2x20 socket for the Pi turned components-down, its standoffs'),
    ('power', 'Power and protection', 'polyfuses, the ideal diode, the 3.3 V regulator'),
    ('panel', 'Fan, LED, stock switch, rear cable', 'the fan header and MOSFET, the activity LED, the stock jumper, '
              'USB from the Pi, the FFC to the rear connector card, Apple\'s card supports'),
]

# keep-out areas on the card's top side (x0, y0, x1, y1, name, what is not allowed)
def keepouts():
    xs = [pi_xy(u, 0)[0] for u in (0, 65)]
    ys = [pi_xy(0, v)[1] for v in (0, 56)]
    k = []
    # under the Pi 3A+ (its USB, HDMI, audio, camera and display connectors hang 5.5-7 mm towards the card);
    # the socket's own strip is left out
    k.append((min(xs), min(ys), max(xs), max(ys), 'PI_3AP_UNDER', 'footprints'))
    # the 3B/3B+'s footprint beyond the 3A+'s, with its USB and RJ45, plus 2 mm (bench use only, see README)
    x3 = [pi_xy(u, 0)[0] for u in (65, 87.1)]
    k.append((min(x3) - 0.5, min(ys) - 1.0, max(x3) + 2.0, min(max(ys) + 2.0, APPLE_BODY_H), 'PI3B_EXT', 'footprints'))
    for (x, y) in APPLE_HOLES:
        k.append((x - 4.5, y - 4.5, x + 4.5, y + 4.5, 'APPLE_SUPPORT', 'footprints'))
    return k


def pi_socket_band():
    """The strip of the PI_3AP_UNDER keep-out that the socket itself occupies (excluded from that keep-out)."""
    x0, y0 = pi_header_pin(39)
    x1, y1 = pi_header_pin(2)
    return (min(x0, x1) - 2.6, min(y0, y1) - 1.6, max(x0, x1) + 2.6, max(y0, y1) + 1.6)


if __name__ == '__main__':
    parts = build()
    print(len(parts), 'parts')
    print('pin 1', pi_header_pin(1), 'pin 2', pi_header_pin(2), 'pin 40', pi_header_pin(40))
    print('plug A1', plug_pin_xy('A', 1), 'C32', plug_pin_xy('C', 32))
    print('limit at Pi rear edge y=%.2f: %.2f vs Pi top %.2f' % (PI_Y0, height_limit(PI_Y0), PI_TOP))
