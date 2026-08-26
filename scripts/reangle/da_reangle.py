"""Depth-Anything-V2 reangle: estimate monocular depth of the drawing (pixel-aligned,
no mesh/matte-alignment), then parallax inverse-warp the original by that depth.
Same warp math as depthwarp.py -> apples-to-apples vs the DrawingSpinUp mesh depth.

Usage: python da_reangle.py <texture.png> <out_dir> [angles] [amp] [encoder]
"""
import sys, os, math
sys.path.insert(0, "/workspace/Depth-Anything-V2")
import numpy as np, torch, cv2
from PIL import Image
from depth_anything_v2.dpt import DepthAnythingV2

TEX = sys.argv[1]; OUT = sys.argv[2]
ANGLES = [float(a) for a in sys.argv[3].split(",")] if len(sys.argv) > 3 else [-18,-15,-12,-9,-6,-3,0,3,6,9,12,15,18]
AMP = float(sys.argv[4]) if len(sys.argv) > 4 else 0.13
ENC = sys.argv[5] if len(sys.argv) > 5 else "vitl"
CFG = {"vits": dict(encoder="vits", features=64,  out_channels=[48,96,192,384]),
       "vitb": dict(encoder="vitb", features=128, out_channels=[96,192,384,768]),
       "vitl": dict(encoder="vitl", features=256, out_channels=[256,512,1024,1024])}[ENC]
os.makedirs(OUT, exist_ok=True)
dev = "cuda"

model = DepthAnythingV2(**CFG)
model.load_state_dict(torch.load(f"/workspace/Depth-Anything-V2/checkpoints/depth_anything_v2_{ENC}.pth", map_location="cpu"))
model = model.to(dev).eval()

tex = np.asarray(Image.open(TEX).convert("RGBA")); th, tw = tex.shape[:2]
alpha = tex[:, :, 3]; rgb = tex[:, :, :3]
al = (alpha[..., None] / 255.0)
onwhite = (rgb * al + 255 * (1 - al)).astype(np.uint8)
with torch.no_grad():
    depth = model.infer_image(cv2.cvtColor(onwhite, cv2.COLOR_RGB2BGR))  # HxW, larger = nearer

m = alpha > 20
d = depth.astype(np.float32)
dv = d[m]; d = (d - dv.min()) / (dv.max() - dv.min() + 1e-6)   # 0..1, 1 = nearest
d[~m] = 0
depth_al = cv2.GaussianBlur(d, (0, 0), 3)
Image.fromarray((depth_al * 255).astype(np.uint8)).save(os.path.join(OUT, "_depth.png"))

A = (alpha.astype(np.float32) / 255.0)[..., None]
gy, gx = np.mgrid[0:th, 0:tw].astype(np.float32)
for ang in ANGLES:
    shift = AMP * tw * (depth_al - 0.5) * math.tan(math.radians(ang))
    mapx = (gx - shift).astype(np.float32); mapy = gy
    wrgb = cv2.remap(rgb.astype(np.float32), mapx, mapy, cv2.INTER_LINEAR, borderValue=(255, 255, 255))
    wa = cv2.remap(A[..., 0], mapx, mapy, cv2.INTER_LINEAR, borderValue=0)[..., None]
    comp = wrgb * wa + 255.0 * (1 - wa)
    Image.fromarray(comp.clip(0, 255).astype(np.uint8)).save(os.path.join(OUT, f"warp_{int(round(ang)):+04d}.png"))
print("done", ENC, "angles", ANGLES, "amp", AMP)
