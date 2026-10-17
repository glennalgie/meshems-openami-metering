"""Draft v0.1: 3-phase leakage CT daughter board (Glenn's ask).
3x MD0630 (one per phase), each through its own opto-isolated RS-485 transceiver,
daisy-chained onto ONE shared 2-wire Modbus bus, 3 distinct slave addresses.
This is a DRAFT for the 1h schematic session, not a final design -- open items are
flagged explicitly at the bottom."""
import matplotlib, os
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

C_L = "#8f1d1d"; C_12V = "#8f1d1d"; C_GND = "#2c3b43"; C_UART = "#2f7fd6"
C_BUS = "#7c5ce0"; INK = "#14242b"; MUT = "#5b6d74"; BORD = "#c9d3d5"; FILL = "#f4f7f7"
QBG = "#fff6e0"; QED = "#c98a00"

fig, ax = plt.subplots(figsize=(11.6, 8.4))
ax.set_xlim(0, 116); ax.set_ylim(0, 84); ax.axis("off")


def box(x, y, w, h, title, sub="", fc=FILL, ec=BORD, tfs=8.6, sfs=7.0):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.25,rounding_size=1.1",
                 linewidth=1.2, edgecolor=ec, facecolor=fc))
    ax.text(x + w / 2, y + h - 2.6, title, ha="center", va="top", fontsize=tfs, fontweight="bold", color=INK)
    if sub:
        ax.text(x + w / 2, y + h / 2 - 1.6, sub, ha="center", va="center", fontsize=sfs, color=MUT)


def wire(pts, color, lw=2.2):
    ax.plot([p[0] for p in pts], [p[1] for p in pts], color=color, lw=lw, solid_capstyle="round")


def dot(x, y, color):
    ax.plot([x], [y], marker="o", ms=4.5, color=color)


def ct(cx, cy, r=2.6):
    from matplotlib.patches import Circle
    ax.add_patch(Circle((cx, cy), r, fill=False, linewidth=1.8, edgecolor=INK))


ax.text(58, 82, "3-Phase Leakage CT Daughter Board, Draft v0.1 (for the 1h schematic session)",
        ha="center", fontsize=11.2, color=INK, fontweight="bold")
ax.text(58, 79.2, "3x IVY MD0630 (1 per phase), each via its own opto-isolated RS-485 transceiver, daisy-chained onto ONE shared Modbus bus",
        ha="center", fontsize=8.0, color=MUT)

# ---- shared 12V / GND rail across the top ----
wire([(6, 74), (104, 74)], C_12V, 2.2)
wire([(6, 71.5), (104, 71.5)], C_GND, 2.2)
ax.text(2.5, 74, "+12V", ha="right", va="center", fontsize=7.2, color=C_12V, fontweight="bold")
ax.text(2.5, 71.5, "GND", ha="right", va="center", fontsize=7.2, color=C_GND, fontweight="bold")
ax.text(58, 76.3, "shared 12V supply + common ground, same rail that feeds the Street-EMS cabinet",
        ha="center", fontsize=6.8, color=MUT)

rows = [
    (58, "L1", "Addr 101 (proposed)"),
    (36, "L2", "Addr 102 (proposed)"),
    (14, "L3", "Addr 103 (proposed)"),
]

bus_x = 90  # shared RS-485 A/B bus, vertical trunk
for (y, phase, addr) in rows:
    # phase line through the CT window (schematic only, not the mains conductor itself)
    ax.text(5, y + 7.2, f"{phase} (through CT window)", ha="left", fontsize=7.0, color=C_L)
    ct(10, y + 4.2)
    ax.text(10, y + 4.2, "CT", ha="center", va="center", fontsize=6.0, color=INK)
    wire([(4, y + 4.2), (7.4, y + 4.2)], C_L, 2.0)
    wire([(12.6, y + 4.2), (16, y + 4.2)], C_L, 2.0)

    # MD0630 block
    box(16, y, 24, 9.5, "IVY MD0630", f"UART TX/RX  |  {addr}", tfs=8.2, sfs=6.6)
    # power taps
    wire([(20, y + 9.5), (20, 74)], C_12V, 1.3)
    wire([(24, y), (24, 71.5)], C_GND, 1.3)
    dot(20, 74, C_12V); dot(24, 71.5, C_GND)

    # UART to isolated transceiver
    wire([(40, y + 6.5), (46, y + 6.5)], C_UART, 2.0)
    ax.text(43, y + 7.6, "TX", ha="center", fontsize=6.2, color=C_UART)
    wire([(40, y + 3), (46, y + 3)], C_UART, 2.0)
    ax.text(43, y + 1.9, "RX", ha="center", fontsize=6.2, color=C_UART)

    # isolated RS-485 transceiver module
    box(46, y, 26, 9.5, "Isolated RS-485", "TTL ↔ RS-485 (opto-isolated)\n(Waveshare-style module)", tfs=8.0, sfs=6.4)
    wire([(50, y + 9.5), (50, 74)], C_12V, 1.3)
    wire([(54, y), (54, 71.5)], C_GND, 1.3)
    dot(50, 74, C_12V); dot(54, 71.5, C_GND)

    # A/B pair to the shared bus trunk
    wire([(72, y + 6.5), (bus_x, y + 6.5)], C_BUS, 2.0)
    wire([(72, y + 3), (bus_x, y + 3)], C_BUS, 2.0)
    ax.text(74, y + 7.6, "A", ha="left", fontsize=6.2, color=C_BUS)
    ax.text(74, y + 1.9, "B", ha="left", fontsize=6.2, color=C_BUS)
    dot(bus_x, y + 6.5, C_BUS); dot(bus_x, y + 3, C_BUS)

# shared RS-485 A/B bus trunk, vertical
wire([(bus_x, 10), (bus_x, 68)], C_BUS, 3.0)
wire([(bus_x + 2, 10), (bus_x + 2, 68)], C_BUS, 3.0)
ax.text(bus_x + 5, 68, "A", ha="left", fontsize=7.0, color=C_BUS, fontweight="bold")
ax.text(bus_x + 5, 10, "B", ha="left", fontsize=7.0, color=C_BUS, fontweight="bold")

# connector out to the Street-EMS RS-485 bus
box(bus_x - 6, 2, 26, 8, "To Street-EMS", "RS-485 bus (daisy-chain out)\n1 bus, 3 node addresses", tfs=7.6, sfs=6.2)
wire([(bus_x, 10), (bus_x - 2, 10)], C_BUS, 2.2)

ax.text(bus_x + 10, 40, "optional 120Ω\nend-of-bus\ntermination\n(open item below)",
        ha="left", fontsize=6.4, color=MUT)

# ---- open items callout ----
box(2, 2, 72, 12, "Open items for the 1h session", fc=QBG, ec=QED, tfs=8.4)
ax.text(38, 6.0,
        "1) Exact slave addresses (101/102/103 shown here vs the 50/51/52 range mentioned earlier)\n"
        "2) Termination resistor: on this board, or only at the far end of the Street-EMS bus?\n"
        "3) Isolated-module part number + footprint (Waveshare reference vs alternative)\n"
        "4) Board outline confirmed at 20 x 8 cm, mounts upside-down under the cabinet top surface",
        ha="center", va="center", fontsize=6.6, color=INK, linespacing=1.9)

plt.tight_layout()
out = os.path.join(os.path.dirname(__file__), "wiring_3ct_rs485_daisychain.png")
tmp = out + ".tmp"
plt.savefig(tmp, dpi=200, bbox_inches="tight", format="png")
os.replace(tmp, out)
print("saved", out)
