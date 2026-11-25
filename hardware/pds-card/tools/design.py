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
# 25 November 2026: four layers (the owner: "I don't mind 4 layer if it is simpler to route and produce"), the
# cheap-tier JLCPCB stackup JLC04161H-7628 (PCBWay's standard 1.6 mm 4-layer is close): 1 oz outer, 0.5 oz inner,
# one 7628 prepreg each side of a 1.065 mm core. L1 and L4 signals, L2 solid GND, L3 +3.3 V. Through vias only.
STACKUP = [  # (name, kind, thickness mm, material, epsilon_r)
    ('F.Mask', 'Top Solder Mask', 0.01, '', 3.3),
    ('F.Cu', 'copper', 0.035, '', 0),
    ('dielectric 1', 'prepreg', 0.2104, '7628', 4.4),
    ('In1.Cu', 'copper', 0.0152, '', 0),
    ('dielectric 2', 'core', 1.065, 'FR4', 4.6),
    ('In2.Cu', 'copper', 0.0152, '', 0),
    ('dielectric 3', 'prepreg', 0.2104, '7628', 4.4),
    ('B.Cu', 'copper', 0.035, '', 0),
    ('B.Mask', 'Bottom Solder Mask', 0.01, '', 3.3),
]
PLANES = {'In1.Cu': 'GND', 'In2.Cu': '+3V3'}   # the inner layers: solid planes, no signals
CLOCK_NETS = ['PI_CLK_GPIO', 'PI_CLK', 'SE_C8M', 'M68K_CLK']   # on L1 only, over L2's unbroken ground
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
# 25 November 2026 (pds-card-3): the Pi beside the slot plug, so that the Pi-speed lines (its data bus, which the
# latches share, and the latch strobes) and the SE's lines are both short, as on the PiStorm.
PI_X1 = 198.7                         # the Pi's u = 0 edge (its microSD end), as close to J1 as its standoffs allow:
                                      # the u = 3.5 holes at x 195.2 end at 197.95, J1's courtyard starts at 198.04
PI_Y1 = 109.2                         # its v = 0 edge (HDMI, audio) at the card's front: rear edge 53.2, the slope's limit
def pi_xy(u, v):
    """The Pi turned components-down about its short axis: u runs to the left (-x), v towards the rear (-y). So
    the header (v = 52.5) lies along the Pi's rear edge, the USB socket (u = 65) at its left-hand end, and a Pi
    3B/3B+'s USB and RJ45 (u 65-88) and their plugs reach away from J1, to the empty left of the card. The price:
    the HDMI and audio sockets, which stand out 1.4 and 2.5 mm past the board's v = 0 edge, overhang the card's
    front edge (README, "Placement")."""
    return PI_X1 - u, PI_Y1 - v
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
    ('USB-A', 52.5, 67.0, 24.5, 38.5, 7.0),     # the drawing's USB-A body: u 53.1-65.9, v 24.2-37.8
    ('HDMI', 24.5, 39.5, -1.4, 11.5, 6.5),      # (the drawing: HDMI 1.4 mm past the edge, audio 2.5, micro-USB 1.1)
    ('audio', 50.0, 57.0, -2.5, 12.0, 6.0), ('camera', 42.0, 48.0, 0.0, 22.0, 5.5),
    ('display', 1.0, 6.0, 15.0, 37.0, 5.5), ('header', 7.0, 58.0, 50.0, 55.0, 8.5),
    ('micro-USB', 6.5, 14.5, -1.1, 4.5, 3.0), ('SoC + heatsink', 20.5, 34.0, 24.2, 38.3, 7.4)]
# the Pi's own underside (now facing up): the microSD socket and its card, at the u = 0 end
PI_UNDERSIDE = [('microSD socket and card', -2.5, 15.0, 20.0, 36.0, 1.6)]
# the 3B / 3B+ beyond the 3A+'s 65 mm: their USB pairs and RJ45 at u = 65..87 (drawing: Z 16.0 and 13.5)
# (Raspberry Pi's 3B+ drawing, measured: the USB pairs at u 69.3-87.8, v 22.4-53.9, the RJ45 at u 65.8-87.4, v 2.1-17.8;
# both stand 2.4-2.8 mm past the board's 85 mm edge)
PI3B_EXT = [('3B/3B+ USB-A pairs', 69.3, 87.8, 22.4, 53.9, 16.0), ('3B/3B+ RJ45', 65.8, 87.4, 2.1, 17.8, 13.5)]
PI3B_PORT_END = 87.8                  # the ports' mouths
PI3B_PLUG_ZONE = 40.0                 # room in front of them for a USB-A plug or an RJ45 plug with its strain relief
KEEP_MARGIN = 2.0
PI3B_ALLOWANCE = 0.42                 # the owner accepts the 3B's USB stack 0.42 mm over Apple's 15.58 mm
# The USB plug in the 3A+'s socket (the pigtail to J5): a right-angle USB-A plug whose head stands about 20 mm out of
# the socket's mouth (u 67) and 16 mm wide; its cable leaves towards the card's rear, to J5. Not a part of the card, but
# the fan must clear it.
PI_USB_PLUG = ('USB-A plug in the Pi\'s socket', 67.0, 87.0, 23.25, 39.25)

# --------------------------------------------------------------------------------------------------------
# The fan: the owner's 30 mm blower (its dimensioned drawing and a photo, 25 November 2026). Body 30.0 +-0.5 mm
# square, 7.0 +-0.5 mm thick. Three 2.1 mm holes: in the drawing's view (X right, Y up, from the body's bottom-left
# corner) two along the bottom edge 24.9 apart and one 23.1 above the bottom-right one; none at the top-left. The
# drawing gives no hole-to-edge offsets: 2.55 mm from the sides and the bottom edge are ASSUMED (the owner to measure).
# The outlet is the whole top edge, about 25 mm from the no-hole corner to the lone hole's lug (the photo); the lead
# leaves the left edge near the bottom-left hole, 50-70 mm long to a JST-SH plug (60 mm assumed).
# On the card the fan is turned so its outlet edge faces the Pi (+x): drawing Y runs along +x, drawing X along +y.
# --------------------------------------------------------------------------------------------------------
FAN_SIZE = 30.0
FAN_T, FAN_T_MAX = 7.0, 7.5
FAN_HOLE_D = 2.2                      # for M2 screws or plastic pins through the fan's 2.1 mm holes
FAN_EDGE = 2.55                       # ASSUMED hole centre to body edge (the owner to measure)
FAN_HOLES_DRAWING = [(FAN_EDGE, FAN_EDGE), (FAN_EDGE + 24.9, FAN_EDGE), (FAN_EDGE + 24.9, FAN_EDGE + 23.1)]
FAN_OUTLET_DRAWING = (0.0, 25.0)      # along the top edge (Y = 30), X from the no-hole corner to the lone hole's lug
FAN_LEAD = 60.0                       # assumed length of its lead
# West of the Pi, in the 3B's plug zone, its outlet aimed at the channel under the Pi between the header (and the
# standoff H6) and the USB socket, behind the USB plug's head (whose cable bends to the rear, past the fan's outlet)
FAN_X0, FAN_Y0 = 78.0, 52.5           # the body's rear-left corner on the card
def fan_xy(X, Y):
    """A point of the fan's drawing (X right, Y up from its bottom-left corner) on the card."""
    return FAN_X0 + Y, FAN_Y0 + X
FAN_AT = (FAN_X0 + FAN_SIZE / 2, FAN_Y0 + FAN_SIZE / 2)
def fan_holes():
    return [fan_xy(X, Y) for X, Y in FAN_HOLES_DRAWING]
def fan_outlet():
    """The outlet on the card: (x, y0, y1), the edge facing the Pi."""
    return FAN_X0 + FAN_SIZE, FAN_Y0 + FAN_OUTLET_DRAWING[0], FAN_Y0 + FAN_OUTLET_DRAWING[1]
def fan_lead_exit():
    return fan_xy(0.0, FAN_EDGE)        # the drawing's left edge (X = 0) beside its bottom-left hole: the rear edge on the card
def fan_notch():
    """The cut in the 3B keep-out that the fan stands in (its courtyard, to the keep-out's rear edge)."""
    return (FAN_X0 - 0.25, FAN_Y0 - 0.25, FAN_X0 + FAN_SIZE + 0.25, FAN_Y0 + FAN_SIZE + 0.25)

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
    'JST_SH_SM04B-SRSS-TB_1x04-1MP_P1.00mm_Horizontal': ('kicad:Connector_JST', 3.0),  # JST: 2.9 mm
    'PinHeader_1x04_P2.54mm_Horizontal': ('kicad:Connector_PinHeader_2.54mm', 3.8),
    'TE_1-84952-0_1x10-1MP_P1.0mm_Horizontal': ('kicad:Connector_FFC-FPC', 2.6),
    'Aurora68_PiSocket_2x20_Inverted': ('generated', SOCKET_H),
    'Aurora68_DIN41612_C_3x32_Male_Straight_PressFit_Bottom': ('generated', 1.5),   # press-fit tails on top
    'Aurora68_AppleCardSupport_3.86mm': ('generated', 0.0),
    'Aurora68_PiStandoff_M2.5': ('generated', 0.0),
    'Aurora68_Fan_Blower_30x30_3xM2': ('generated', 7.5),
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
    'BAT54C': (lambda: [('1', 'A1', 'passive'), ('2', 'A2', 'passive'), ('3', 'K', 'passive')], 'D',
               'Dual Schottky diode, common cathode, SOT-23 (1 A1, 2 A2, 3 K)', ''),
    'Fan_Blower_30mm': (lambda: [], 'M', '30 mm 5 V blower fan, held by three M2 screws; its lead plugs into J3', ''),
    'R': (_pins_two, 'R', 'Resistor', ''),
    'C': (_pins_two, 'C', 'Capacitor', ''),
    'Polyfuse': (_pins_two, 'F', 'Resettable fuse (PTC)', ''),
    'LED': (lambda: [('1', 'K', 'passive'), ('2', 'A', 'passive')], 'D', 'LED', ''),
    'Conn_01x02': (lambda: [('1', 'Pin_1', 'passive'), ('2', 'Pin_2', 'passive')], 'J', 'Header 1x02', ''),
    'Conn_01x03': (lambda: [(str(i), 'Pin_%d' % i, 'passive') for i in (1, 2, 3)], 'J', 'Header 1x03', ''),
    'Conn_01x04': (lambda: [(str(i), 'Pin_%d' % i, 'passive') for i in (1, 2, 3, 4)], 'J', 'Header 1x04', ''),
    'Conn_01x04_MP': (lambda: [(str(i), 'Pin_%d' % i, 'passive') for i in (1, 2, 3, 4)] + [('MP', 'MP', 'passive')], 'J',
                      'Connector, 4 positions, 1.0 mm (JST-SH), with mounting pads', ''),
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
    'M68K_RESET_n': 67, 'M68K_BR_n': 69, 'M68K_BGACK_n': 70, 'M68K_BG_n': 71,
    'M68K_AS_n': 72, 'M68K_UDS_n': 73, 'M68K_LDS_n': 74, 'M68K_RW': 75, 'M68K_DTACK_n': 76,
    # new on Card P (PiStorm: free pins)
    'SLOT_PWR': 77, 'FAN_PWM': 78, 'FAN_TACH': 81, 'LED_ACT': 83,
}
PISTORM_PIN_NOTES = {53: "the PiStorm's E pin (an output there)",
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
    nets[str(GPIO_PIN[4])] = 'PI_CLK_GPIO'             # GPCLK0 reaches the CPLD through R12, 22 ohm at the Pi's pin
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
        if n[k] in ('+12V_SLOT', '-5V_SLOT', '-12V_SLOT', 'SE_C16M', 'SE_HALT_n', 'SE_EXTDTK_n', 'SE_PMCYC_n'):
            n[k] = None                 # not used: no +12/-5/-12 V loads, C16M unused (C8M gives the S-states), /HALT is
                                        # /RESET, /EXT.DTK left alone, /PMCYC not needed (docs/22 section 10)
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
    cbtd('U6', U6_AT, [('SE_E', 'M68K_E'), ('SE_IPL0_n', 'M68K_IPL_n0'), ('SE_IPL1_n', 'M68K_IPL_n1'),
                               ('SE_IPL2_n', 'M68K_IPL_n2'), ('SE_BERR_n', 'M68K_BERR_n'), ('SE_RESET_n', 'M68K_RESET_n'),
                               (None, None), ('SE_AS_n', 'M68K_AS_n'), ('SE_UDS_n', 'M68K_UDS_n'),
                               ('SE_LDS_n', 'M68K_LDS_n')])
    cbtd('U7', U7_AT, [('SE_RW', 'M68K_RW'), ('SE_DTACK_n', 'M68K_DTACK_n'), ('SE_BG_n', 'M68K_BG_n'),
                                ('SE_BGACK_n', 'M68K_BGACK_n'), ('SE_BR_n', 'M68K_BR_n'), ('SE_VMA_n', 'M68K_VMA_n'),
                                ('SE_VPA_n', 'M68K_VPA_n'), ('SE_FC0', 'M68K_FC0'), ('SE_FC1', 'M68K_FC1'),
                                ('SE_FC2', 'M68K_FC2')])
    add(Part('C5', 'C', '100n', 'C_0603_1608Metric', 'slot', (179.5, 79.0), {'1': '5V_PI', '2': 'GND'}, rot=90, lcsc='C14663'))
    add(Part('C6', 'C', '100n', 'C_0603_1608Metric', 'slot', (179.5, 89.0), {'1': '5V_PI', '2': 'GND'}, rot=90, lcsc='C14663'))
    add(Part('R3', 'R', '22', 'R_0603_1608Metric', 'slot', (197.0, 36.0), {'1': 'SE_C8M', '2': 'M68K_CLK'}, rot=90,
             lcsc='C23345', note='C8M into the CPLD, as the PiStorm\'s 68000 clock (its R3)'))
    add(Part('R4', 'R', '10k', 'R_0603_1608Metric', 'slot', (192.0, 30.5), {'1': '+5V_SLOT', '2': 'SLOT_PWR'}, lcsc='C25804',
             note='SE power sense: 5 V -> 3.0 V'))
    add(Part('R5', 'R', '15k', 'R_0603_1608Metric', 'slot', (192.0, 33.0), {'1': 'SLOT_PWR', '2': 'GND'}, lcsc='C22809'))

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
    # beside the Pi's header, under the Pi (components down 8.5 mm above), in the pocket between the keep-outs
    # under its SoC and its USB socket: Pi-to-CPLD traces of about 25 mm, as on the PiStorm where the Pi sits over
    # the CPLD (PLACEMENT in the README)
    add(Part('U1', 'EPM240_570_T100', 'EPM240T100C5N', 'TQFP-100_14x14mm_P0.5mm', 'cpld', U1_AT, cp, rot=U1_ROT,
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
    latch('U2', (185.45, 67.0), 180, pid[0:8], pid[8:16], a[0:8], a[8:16], 'LTCH_A_0', 'LTCH_A_8', 'LTCH_A_OE_n', 'LTCH_A_OE_n')
    latch('U3', (192.2, 45.0), 180, pid[0:8], pid[8:16], a[16:24], [None] * 8, 'LTCH_A_16', 'LTCH_A_24', 'LTCH_A_OE_n', 'LTCH_A_OE_n')
    latch('U4', (182.6, 45.0), 180, pid[0:8], pid[8:16], d[0:8], d[8:16], 'LTCH_D_WR_L', 'LTCH_D_WR_U', 'LTCH_D_WR_OE_n', 'LTCH_D_WR_OE_n')
    latch('U5', (173.0, 45.0), 0, d[0:8], d[8:16], pid[0:8], pid[8:16], 'LTCH_D_RD_L', 'LTCH_D_RD_U', 'LTCH_D_RD_OE_n', 'LTCH_D_RD_OE_n')
    for ref, at in (('C1', (179.5, 67.0)), ('C2', (192.2, 36.5)), ('C3', (182.6, 36.5)), ('C4', (173.0, 36.5))):
        add(Part(ref, 'C', '100n', 'C_0603_1608Metric', 'cpld', at, {'1': '+3V3', '2': 'GND'}, rot=90, lcsc='C14663',
                 note='latch decoupling (PiStorm C1-C4)'))
    for ref, at, r in (('C7', (149.5, 79.2), 0), ('C8', (153.5, 79.2), 0), ('C9', (157.5, 79.2), 0), ('C10', (161.5, 79.2), 0),
                       ('C11', (166.5, 66.0), 90), ('C12', (166.5, 62.5), 90)):
        add(Part(ref, 'C', '100n', 'C_0603_1608Metric', 'cpld', at, {'1': '+3V3', '2': 'GND'}, rot=r, lcsc='C14663',
                 note='CPLD VCCIO/VCCINT decoupling (PiStorm C7-C10, C13, C14)'))
    add(Part('R1', 'R', '4.7k', 'R_0603_1608Metric', 'cpld', (169.0, 66.0), {'1': '+3V3', '2': 'JTAG_TMS'}, rot=90, lcsc='C23162',
             note='TMS pull-up (PiStorm R2)'))
    add(Part('R2', 'R', '4.7k', 'R_0603_1608Metric', 'cpld', (169.0, 62.5), {'1': '+3V3', '2': 'JTAG_TDI'}, rot=90, lcsc='C23162',
             note='TDI pull-up (PiStorm R1)'))

    # ---- the Pi ----
    x7, y7 = pi_header_pin(GPIO_PIN[4])
    add(Part('R12', 'R', '22', 'R_0603_1608Metric', 'pi', R12_AT, {'1': 'PI_CLK_GPIO', '2': 'PI_CLK'}, rot=180,
             lcsc='C23345', note='GPCLK0 (200 MHz) series resistor at the source, beside the Pi\'s pin 7 (fit 0 ohm for the PiStorm\'s direct wire)'))
    add(Part('J2', 'RPI_GPIO_SOCKET_2x20', 'Pi socket 2x20 6.0 mm', 'Aurora68_PiSocket_2x20_Inverted', 'pi',
             pi_header_pin(1), pi_socket_nets(),
             mpn='2x20 female socket, 2.54 mm, 6.0 mm body, open-bottom contacts accepting 6.0 mm pins (choose; README)'))
    for i, (x, y) in enumerate(PI_HOLES_3AP + PI_HOLES_Z2W, start=3):
        add(Part('H%d' % i, 'MountingHole', 'M2.5 standoff 8.5 mm' if i < 7 else 'M2.5 standoff 8.5 mm (Zero 2 W)',
                 'Aurora68_PiStandoff_M2.5', 'pi', (x, y), {}))

    # ---- power: 5 V from the slot through a polyfuse and an ideal diode to the Pi and the card ----
    add(Part('F1', 'Polyfuse', '1.1A hold', 'Fuse_1812_4532Metric', 'power', (180.0, 25.0), {'1': '+5V_SLOT', '2': '5V_F'},
             mpn='PTC 1812, 1.1 A hold, 2.2 A trip, 6 V or more (e.g. Bourns MF-MSMF110-2)'))
    add(Part('U9', 'LM66100DCK', 'LM66100', 'SOT-363_SC-70-6', 'power', (172.0, 25.0),
             {'1': '5V_F', '2': 'GND', '3': 'GND', '4': None, '5': None, '6': '5V_PI'}, mpn='LM66100DCKR',
             note='ideal diode: a Pi powered from its own USB never feeds the SE'))
    add(Part('C13', 'C', '10u', 'C_0805_2012Metric', 'power', (172.0, 28.5), {'1': '5V_F', '2': 'GND'}, lcsc='C15850'))
    add(Part('C14', 'C', '22u', 'C_0805_2012Metric', 'power', (167.0, 25.0), {'1': '5V_PI', '2': 'GND'}, rot=90, lcsc='C45783'))
    add(Part('U8', 'AMS1117-3.3', 'AMS1117-3.3', 'SOT-223-3_TabPin2', 'power', (159.0, 27.5),
             {'1': 'GND', '2': '+3V3', '3': '5V_PI'}, lcsc='C6186', note='PiStorm U8 (LM1117-3.3)'))
    add(Part('C15', 'C', '22u', 'C_0805_2012Metric', 'power', (152.5, 25.5), {'1': '5V_PI', '2': 'GND'}, rot=90, lcsc='C45783',
             note='PiStorm C11'))
    add(Part('C16', 'C', '22u', 'C_0805_2012Metric', 'power', (152.5, 30.5), {'1': '+3V3', '2': 'GND'}, rot=90, lcsc='C45783',
             note='PiStorm C12'))
    add(Part('F2', 'Polyfuse', '0.35A hold', 'Fuse_1206_3216Metric', 'power', (180.0, 30.5), {'1': '+5V_SLOT', '2': '5V_FAN'},
             mpn='PTC 1206, 0.35 A hold, 6 V or more'))
    add(Part('F3', 'Polyfuse', '0.75A hold', 'Fuse_1812_4532Metric', 'power', (187.5, 25.0), {'1': '+5V_SLOT', '2': '5V_PANEL'},
             mpn='PTC 1812, 0.75 A hold (Apple allows 750 mA at +5 V for a peripheral outside the case)'))

    # ---- the fan: the owner's 30 mm 5 V blower (M1, optional) on a JST-SH header wired as the Raspberry Pi 5's ----
    # FAN connector (Raspberry Pi's documentation: 1 +5 V, 2 PWM, 3 GND, 4 tachometer). PWM comes straight from a CPLD
    # pin (through 100 ohm), which drives it high from power-up: full speed with no software, as JP1 used to give; while
    # the CPLD is blank or being programmed its pins float with weak pull-ups, and a 4-wire fan's own pull-up runs it at
    # full speed then too. D1 holds PWM and TACH at 3.3 V if a fan pulls them up to 5 V (the MAX II's inputs stop at 4.0).
    add(Part('J3', 'Conn_01x04_MP', 'Fan (JST-SH 4)', 'JST_SH_SM04B-SRSS-TB_1x04-1MP_P1.00mm_Horizontal', 'panel', J3_AT,
             {'1': '5V_FAN', '2': 'FAN_PWM_C', '3': 'GND', '4': 'FAN_TACH', 'MP': 'GND'}, rot=J3_ROT, lcsc='C160390',
             mpn='JST SM04B-SRSS-TB(LF)(SN)',
             note='the Raspberry Pi 5 FAN pinout: 1 +5 V, 2 PWM, 3 GND, 4 tachometer (LCSC number to confirm)'))
    add(Part('R8', 'R', '100', 'R_0603_1608Metric', 'panel', R8_AT, {'1': 'FAN_PWM', '2': 'FAN_PWM_C'}, lcsc='C22775',
             note='fan PWM from the CPLD, series'))
    add(Part('D1', 'BAT54C', 'BAT54C', 'SOT-23', 'panel', D1_AT, {'1': 'FAN_PWM_C', '2': 'FAN_TACH', '3': '+3V3'},
             mpn='BAT54C (common cathode)', note='clamps the fan\'s PWM and TACH to 3.3 V (LCSC number to choose)'))
    add(Part('R6', 'R', '10k', 'R_0603_1608Metric', 'panel', R6_AT, {'1': '+3V3', '2': 'FAN_TACH'}, lcsc='C25804',
             note='tachometer pull-up'))
    add(Part('M1', 'Fan_Blower_30mm', '30x30x7 5 V blower (optional)', 'Aurora68_Fan_Blower_30x30_3xM2', 'panel', FAN_AT, {},
             height=FAN_T_MAX,
             mpn='30 x 30 x 7 mm 5 V blower, about 0.2 A, PWM and tachometer, JST-SH 4-pin lead of 60 mm or more (the owner\'s; '
                 'Raspberry Pi 5 fan pinout); three M2 screws or plastic pins',
             note='optional, fitted by hand; outlet towards the Pi; remove it to use a 3B/3B+ on the bench'))

    # ---- LED, stock switch, USB in, the cable to the rear connector card ----
    add(Part('D2', 'LED', 'green', 'LED_0603_1608Metric', 'panel', (130.0, 33.0), {'1': 'GND', '2': 'LED_A'}, rot=180,
             note='activity'))
    add(Part('R10', 'R', '330', 'R_0603_1608Metric', 'panel', (130.0, 36.5), {'1': 'LED_ACT', '2': 'LED_A'}, lcsc='C23138'))
    add(Part('R11', 'R', '330', 'R_0603_1608Metric', 'panel', (130.0, 30.0), {'1': 'LED_ACT', '2': 'LED_PANEL'}, lcsc='C23138',
             note='the connector card\'s LED, over the cable'))
    add(Part('J5', 'Conn_01x04', 'USB from Pi', 'PinHeader_1x04_P2.54mm_Horizontal', 'panel', (118.0, 47.0),
             {'1': None, '2': 'USB_DM', '3': 'USB_DP', '4': 'GND'}, rot=90,
             note='a short pigtail from the Pi 3A+\'s USB socket, which faces it, with a right-angle USB-A plug whose cable leaves towards the rear (the fan is beside it); pin 1 (VBUS) deliberately not connected'))
    add(Part('J4', 'FFC_01x10_MP', 'Rear connector card', 'TE_1-84952-0_1x10-1MP_P1.0mm_Horizontal', 'panel', (118.0, 26.5),
             {'1': 'GND', '2': 'USB_DM', '3': 'USB_DP', '4': 'GND', '5': '5V_PANEL', '6': '5V_PANEL', '7': 'GND',
              '8': None, '9': 'LED_PANEL', '10': 'GND', 'MP': 'GND'}, rot=180,
             mpn='TE 1-84952-0 (FFC 1.0 mm, 10 positions, ZIF)'))
    # ---- Apple's card supports ----
    for i, (x, y) in enumerate(APPLE_HOLES, start=1):
        add(Part('H%d' % i, 'MountingHole', 'Apple card support 3.86 mm', 'Aurora68_AppleCardSupport_3.86mm', 'panel', (x, y), {}))
    return P


U1_AT = (155.45, 68.85)               # the CPLD under the Pi, in the pocket between its USB and SoC keep-outs
U1_ROT = 180                          # (tools/placement.py compares the four rotations)
U6_AT = (185.45, 79.0)                # the bus switches beside the latches, under the Pi, by J1's control pins
U7_AT = (185.45, 89.0)
R12_AT = (176.5, 61.0)                # GPCLK0's 22 ohm, 5 mm from the Pi's pin 7, on the way to the CPLD's pin 12
J3_AT, J3_ROT = (88.0, 47.5), 0       # the fan's JST-SH header behind the fan, its mouth facing the fan's lead (+y)
R8_AT = (94.0, 44.0)
D1_AT = (94.0, 47.5)
R6_AT = (88.0, 42.0)

SHEETS = [  # file, title, what is on it
    ('slot', 'Slot plug and bus switches', 'J1, the CBTD3384 switches, C8M, the SE power sense'),
    ('cpld', 'CPLD and latches', 'the EPM240, the four 74LVC16373, JTAG pull-ups, decoupling'),
    ('pi', 'Raspberry Pi socket', 'the 2x20 socket for the Pi turned components-down, its standoffs'),
    ('power', 'Power and protection', 'polyfuses, the ideal diode, the 3.3 V regulator'),
    ('panel', 'Fan, LED, rear cable', 'the fan (JST-SH, Raspberry Pi 5 pinout), the activity LED, '
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
    # the 3B/3B+ beyond the 3A+: its board, its USB and RJ45 with their overhang, and 40 mm in front of their mouths
    # for the plugs, all with 2 mm margin, to the card's front edge: no footprints at all (bench use only, see README)
    # (its left edge: the 3B's board starts at x = 77 and its RJ45 at 77.8; the zone starts at 76.3, 0.7 mm short of the
    # board and 1.5 mm short of the RJ45, because the Pi's right-hand standoffs, which a 3B uses too, end at 76.25)
    sgn = 1.0 if pi_xy(1, 0)[0] > pi_xy(0, 0)[0] else -1.0          # which way u runs on the card
    near = pi_xy(65.0, 0)[0] - sgn * KEEP_MARGIN
    hole = pi_xy(61.5, 0)[0] + sgn * (5.5 / 2 + 0.05)              # the standoffs a 3B shares stay out of the zone
    near = max(near, hole) if sgn > 0 else min(near, hole)
    far = pi_xy(PI3B_PORT_END + PI3B_PLUG_ZONE, 0)[0] + sgn * KEEP_MARGIN
    ports = pi_xy(PI3B_PORT_END, 0)[0] + sgn * KEEP_MARGIN
    k.append((min(near, far), min(ys) - KEEP_MARGIN, max(near, far), APPLE_BODY_H, 'PI3B_EXT', 'footprints'))
    # over the board and the ports themselves (not the plug zone) no vias either: the sockets' shells could press there
    k.append((min(near, ports), min(ys) - KEEP_MARGIN, max(near, ports), APPLE_BODY_H, 'PI3B_PORTS', 'vias'))
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
    y0 = min(pi_xy(0, 0)[1], pi_xy(0, PI_D)[1])
    print('limit at Pi rear edge y=%.2f: %.2f vs Pi top %.2f' % (y0, height_limit(y0), PI_TOP))
    print('fan', FAN_AT, 'holes', fan_holes(), 'outlet', fan_outlet(), 'limit at its rear edge %.2f' % height_limit(FAN_Y0))
