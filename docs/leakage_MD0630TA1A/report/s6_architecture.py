"""S6 architecture: MD0630 <-> ESP32 with (A) hardware alarm sense + (B) reverse-flow adaptive threshold."""
import matplotlib, os
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

INK="#14242b"; MUT="#5b6d74"; BORD="#c9d3d5"; FILL="#f4f7f7"
MOD="#cf2f36"; ESP="#2f7fd6"; A="#bf6a12"; B="#1f8a4c"

fig,ax=plt.subplots(figsize=(9.8,5.2)); ax.set_xlim(0,100); ax.set_ylim(0,60); ax.axis("off")

def box(x,y,w,h,title,sub="",edge=BORD,tcol=INK,fs=10.5,subfs=7.8):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle="round,pad=0.3,rounding_size=1.3",
                 linewidth=1.6,edgecolor=edge,facecolor=FILL))
    ax.text(x+w/2,y+h-3.0,title,ha="center",va="top",fontsize=fs,fontweight="bold",color=tcol)
    if sub:
        for i,line in enumerate(sub.split("\n")):
            ax.text(x+w/2,y+h-6.4-i*3.0,line,ha="center",va="top",fontsize=subfs,color=MUT)

def arrow(x0,y0,x1,y1,color=INK,lw=2.0,style="-|>"):
    ax.add_patch(FancyArrowPatch((x0,y0),(x1,y1),arrowstyle=style,mutation_scale=13,lw=lw,color=color,shrinkA=3,shrinkB=3))

# MD0630 (left)
box(2,20,26,22,"MD0630 Type-B RCM","AC + DC residual\nrelay trips on its\nOWN threshold",edge=MOD,tcol=MOD)
# ESP32 (right)
box(72,20,26,22,"ESP32-S3 EMS","OpenAMI firmware\nlog + MQTT",edge=ESP,tcol=ESP)

# B path (top): Modbus digital, adaptive threshold
box(37,42,26,14,"(B) Adaptive threshold","reverse power flow\nP<0  ->  30 -> 27 mA\nFC16 + read-back",edge=B,tcol=B)
arrow(15,42,45,56,B)                       # module -> B (leakage/threshold up)
arrow(50,42,50,42)                          # spacer (noop)
arrow(63,49,72,38,B)                        # B -> ESP32 (write path)
ax.text(64,52,"Modbus RTU",ha="left",fontsize=7.4,color=MUT)

# A path (bottom): hardware alarm lines -> opto -> GPIO
box(37,4,26,14,"(A) Alarm sense","AC / DC / fault out\nopto / level-shift\nGPIO 33/34/21 in",edge=A,tcol=A)
arrow(28,26,37,12,A)                        # module alarm outputs -> opto
arrow(63,11,72,24,A)                        # opto -> ESP32 GPIO (read-only)
ax.text(30,17,"5-12 V",ha="left",fontsize=7.2,color=MUT)
ax.text(64,15,"3.3 V in",ha="left",fontsize=7.2,color=MUT)

# meter feed into B
ax.text(50,39,"meter active_power  (import / export)",ha="center",fontsize=7.4,color=MUT)

ax.text(50,58.5,"S6 — hardware alarm sense (A) + reverse-power-flow adaptive threshold (B)",
        ha="center",fontsize=9.6,color=INK)
plt.tight_layout()
out=os.path.join(os.path.dirname(__file__),"s6_architecture.png")
plt.savefig(out,dpi=200,bbox_inches="tight"); print("saved",out)
