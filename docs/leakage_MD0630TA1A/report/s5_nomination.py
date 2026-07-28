"""Generate the S5 nomination diagram: 3 EMS telemetry -> magnitude + peer-correlation -> gate -> action."""
import matplotlib, os
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

INK="#14242b"; MUT="#5b6d74"; BORD="#c9d3d5"; FILL="#f4f7f7"
NOM="#cf2f36"; PEER="#2f7fd6"; OK="#1f8a4c"; ACC="#bf6a12"

fig,ax=plt.subplots(figsize=(9.7,4.8)); ax.set_xlim(0,100); ax.set_ylim(0,55); ax.axis("off")

def box(x,y,w,h,title,sub="",edge=BORD,tcol=INK,fs=10.5):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle="round,pad=0.3,rounding_size=1.3",
                 linewidth=1.5,edgecolor=edge,facecolor=FILL))
    ax.text(x+w/2,y+h-3.0,title,ha="center",va="top",fontsize=fs,fontweight="bold",color=tcol)
    if sub: ax.text(x+w/2,y+h-6.6,sub,ha="center",va="top",fontsize=7.7,color=MUT)

def arrow(x0,y0,x1,y1,color=INK,lw=2.0):
    ax.add_patch(FancyArrowPatch((x0,y0),(x1,y1),arrowstyle="-|>",mutation_scale=13,lw=lw,color=color,shrinkA=3,shrinkB=3))

# 3 EMS telemetry (left)
box(2,37,26,10,"ems-01 (self)","AC 45 mA  rising",edge=NOM,tcol=NOM)
box(2,23,26,10,"ems-04 (peer)","AC 18 mA",edge=PEER,tcol=PEER)
box(2,9,26,10,"ems-01far (peer)","AC 4.5 mA",edge=PEER,tcol=PEER)

# nomination (title only; sub-lines drawn manually below to control spacing)
box(37,20,30,17,"Nomination round")
ax.text(52,30.0,"rank by magnitude",ha="center",fontsize=7.7,color=MUT)
ax.text(52,27.0,"+ min peer-correlation",ha="center",fontsize=7.7,color=MUT)
ax.text(52,24.0,"(odd-one-out)",ha="center",fontsize=7.7,color=MUT)

# gate
box(74,29,24,10,"confidence gate",">= 0.30 & quorum",edge=ACC,tcol=ACC)
# outcomes
box(74,15,24,9,"ISOLATE","bracket up/down",edge=OK,tcol=OK)
box(74,3,24,8,"alert-only","if too close",edge=BORD,tcol=MUT,fs=9.5)

arrow(28,42,37,33,NOM)     # self -> nomination
arrow(28,28,37,28,PEER)    # peer -> nomination
arrow(28,14,37,23,PEER)    # peer -> nomination
arrow(67,30,74,33,ACC)     # nomination -> gate
arrow(86,29,86,24,OK)      # gate -> isolate
ax.text(88.5,26.5,"conf 0.82",ha="left",fontsize=7.6,color=OK)
arrow(80,15,80,11,MUT)     # (else) -> alert

ax.text(50,52.5,"S5 nomination: magnitude + peer-correlation, confidence-gated (literature-grounded)",
        ha="center",fontsize=9.4,color=INK)
plt.tight_layout()
out=os.path.join(os.path.dirname(__file__),"s5_nomination.png")
plt.savefig(out,dpi=200,bbox_inches="tight"); print("saved",out)
