"""Generate the S4 architecture / data-flow diagram (energy-change correlation cache)."""
import matplotlib, os
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

INK="#14242b"; MUT="#5b6d74"; BORD="#c9d3d5"; FILL="#f4f7f7"
LEAK="#cf2f36"; ENER="#2f7fd6"; OUT="#1f8a4c"

fig,ax=plt.subplots(figsize=(9.6,4.7)); ax.set_xlim(0,100); ax.set_ylim(0,55); ax.axis("off")

def box(x,y,w,h,title,sub="",edge=BORD,tcol=INK):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle="round,pad=0.3,rounding_size=1.4",
                 linewidth=1.5,edgecolor=edge,facecolor=FILL))
    ax.text(x+w/2,y+h-3.2,title,ha="center",va="top",fontsize=10.5,fontweight="bold",color=tcol)
    if sub: ax.text(x+w/2,y+h-7.1,sub,ha="center",va="top",fontsize=8,color=MUT)

def arrow(x0,y0,x1,y1,color=INK,style="-",lw=2.0):
    ax.add_patch(FancyArrowPatch((x0,y0),(x1,y1),arrowstyle="-|>",mutation_scale=14,
                 lw=lw,color=color,linestyle=style,shrinkA=3,shrinkB=3))

# input boxes
box(3,33,25,11,"MD0630 leakage","AC / DC  (mA)",edge=LEAK,tcol=LEAK)
box(3,10,25,11,"EMS energy","readings[0]: V, I, P, E",edge=ENER,tcol=ENER)

# middle
box(38,32,29,13,"LeakageInsightsCache","step detector  (|delta| >= 1 mA)")
box(38,9,29,13,"EnergyRing<128>","record() each poll cycle")

# output
box(75,21,22,13,"LeakageStep","{from->to mA, energy}",edge=OUT,tcol=OUT)

# arrows
arrow(28,38.5,38,38.5,LEAK)                     # leakage -> detector
ax.text(33,40.2,"update(mA)",ha="center",fontsize=7.8,color=LEAK)
arrow(28,15.5,38,15.5,ENER)                     # energy -> ring
ax.text(33,17.2,"record()",ha="center",fontsize=7.8,color=ENER)
arrow(52,32,52,22,MUT,style=(0,(4,3)))          # detector queries ring
ax.text(54,27,"lookback +/- 2 s",ha="left",fontsize=7.8,color=MUT)
arrow(67,38.5,75,30,OUT)                        # on step -> LeakageStep
ax.text(70.5,35.5,"on step",ha="center",fontsize=7.8,color=OUT)
arrow(86,21,86,17,OUT)                          # -> MQTT
ax.text(86,15.5,"toJson -> MQTT (S5)",ha="center",fontsize=8,color=OUT)

ax.text(50,52,"S4 - a leakage step is matched to the energy event around it (+/- window)",
        ha="center",fontsize=9.4,color=INK)
plt.tight_layout()
out=os.path.join(os.path.dirname(__file__),"s4_architecture.png")
plt.savefig(out,dpi=200,bbox_inches="tight"); print("saved",out)
