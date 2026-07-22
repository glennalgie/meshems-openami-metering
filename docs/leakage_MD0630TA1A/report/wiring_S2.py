"""Generate the S2 bench wiring diagram (ESP32 reads the MD0630 directly on its UART, 3.3V pull-ups)."""
import matplotlib, os
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

C_12V="#8f1d1d"; C_3V3="#7c5ce0"; C_GND="#2c3b43"; C_TX="#2f7fd6"; C_RX="#c98a00"
INK="#14242b"; MUT="#5b6d74"; BORD="#c9d3d5"; FILL="#f4f7f7"

fig,ax=plt.subplots(figsize=(9.4,4.7)); ax.set_xlim(0,100); ax.set_ylim(0,55); ax.axis("off")

def box(x,y,w,h,title,sub=""):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle="round,pad=0.3,rounding_size=1.2",
                 linewidth=1.3,edgecolor=BORD,facecolor=FILL))
    ax.text(x+w/2,y+h-3.4,title,ha="center",va="top",fontsize=11,fontweight="bold",color=INK)
    if sub: ax.text(x+w/2,y+h-7.4,sub,ha="center",va="top",fontsize=8,color=MUT)

def wire(pts,color,lw=2.4):
    ax.plot([p[0] for p in pts],[p[1] for p in pts],color=color,lw=lw,solid_capstyle="round")
def dot(x,y,color): ax.plot([x],[y],marker="o",ms=5,color=color)
def resistor(x,y0,y1,label,ldx=2.6):
    n=6; xs=[x]; ys=[y0]
    for i in range(n):
        ys.append(y0-(y0-y1)*(i+1)/(n+1)); xs.append(x+(2 if i%2==0 else -2))
    xs.append(x); ys.append(y1)
    ax.plot(xs,ys,color=C_3V3,lw=1.8)
    ax.text(x+ldx,(y0+y1)/2,label,fontsize=7.5,color=C_3V3,va="center",
            ha=("left" if ldx>0 else "right"))

# components
box(4,20,17,15,"12 V PSU","limit 50-100 mA")
box(38,17,19,20,"IVY MD0630","UART - TX open-drain")
box(70,14,26,29,"ESP32-S3","-> PC: flash + monitor")

# 12 V to module VCC
wire([(21,30),(38,30)],C_12V,3); ax.text(29.5,31.6,"12 V",ha="center",fontsize=9,color=C_12V,fontweight="bold")
ax.text(39.5,27.5,"VCC",ha="left",va="center",fontsize=8,color=INK)

# 3.3 V rail from the ESP32 (kept clear of every box)
wire([(72,38),(72,40),(58,40)],C_3V3,2.6)
ax.text(64,41.4,"+3.3 V rail  (pull-ups)",ha="center",fontsize=8,color=C_3V3)

# data lines in the gap between module and ESP32 (crossed)
wire([(57,30),(66,30),(66,27),(70,27)],C_TX,2.4)   # module TX -> ESP32 GPIO42 (RX)
wire([(70,23),(69,23),(69,25),(57,25)],C_RX,2.4)   # ESP32 GPIO7 (TX) -> module RX
ax.text(58.5,33.4,"TX -> GPIO42",ha="left",fontsize=7.5,color=C_TX)
ax.text(58.5,21.8,"RX <- GPIO7",ha="left",fontsize=7.5,color=C_RX)
ax.text(71.5,27,"42",ha="left",va="center",fontsize=8,color=INK)
ax.text(71.5,23,"7",ha="left",va="center",fontsize=8,color=INK)

# pull-ups tapping each data line, up to the 3.3 V rail
resistor(60,40,30,"4.7k",ldx=2.4)
resistor(67,40,25,"4.7k",ldx=-2.4)
dot(60,30,C_3V3); dot(67,25,C_3V3)

# common ground
wire([(12,13),(88,13)],C_GND,2.6)
for x in (12,47,83,88): dot(x,13,C_GND)
wire([(12,20),(12,13)],C_GND,2.2)   # PSU -
wire([(47,17),(47,13)],C_GND,2.2)   # module GND
wire([(83,14),(83,13)],C_GND,2.2)   # ESP32 GND
ax.text(50,10.4,"COMMON GROUND   —   PSU(-)  ·  module  ·  ESP32",
        ha="center",fontsize=8.5,color=MUT)
ax.text(55.8,30,"TX",ha="right",va="center",fontsize=8,color=INK)
ax.text(55.8,25,"RX",ha="right",va="center",fontsize=8,color=INK)
ax.text(72,18,"GND",ha="left",va="center",fontsize=8,color=INK)

ax.text(50,53.5,"S2 bench: ESP32 reads the MD0630 directly on its UART (GPIO42/7); 3.3 V feeds the pull-ups",
        ha="center",fontsize=9.2,color=INK)
plt.tight_layout()
out=os.path.join(os.path.dirname(__file__),"wiring_S2.png")
plt.savefig(out,dpi=200,bbox_inches="tight"); print("saved",out)
