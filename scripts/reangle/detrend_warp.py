"""Detrend a monocular depth map (remove the global a*x+b*y+c plane = the scene tilt),
keeping only the local front-back relief, then parallax-warp. Tests whether DA-V2 has
usable object relief hiding under its head-near/feet-far gradient.

Usage: python detrend_warp.py <depth.png> <texture.png> <out_dir> [angles] [amp]
"""
import sys, os, math
import numpy as np, cv2
from PIL import Image

DEPTH = sys.argv[1]; TEX = sys.argv[2]; OUT = sys.argv[3]
ANGLES = [float(a) for a in sys.argv[4].split(",")] if len(sys.argv) > 4 else [-18,-15,-12,-9,-6,-3,0,3,6,9,12,15,18]
AMP = float(sys.argv[5]) if len(sys.argv) > 5 else 0.13
os.makedirs(OUT, exist_ok=True)

d = np.asarray(Image.open(DEPTH).convert("L"), np.float32) / 255.0
tex = np.asarray(Image.open(TEX).convert("RGBA")); th, tw = tex.shape[:2]
alpha = tex[:, :, 3]; rgb = tex[:, :, :3]; m = alpha > 20
ys, xs = np.where(m)

# fit plane d ~ a*x + b*y + c over the figure, subtract it
Amat = np.stack([xs, ys, np.ones_like(xs)], 1).astype(np.float32)
coef, *_ = np.linalg.lstsq(Amat, d[m], rcond=None)
plane = (coef[0] * np.arange(tw)[None, :] + coef[1] * np.arange(th)[:, None] + coef[2]).astype(np.float32)
res = d - plane
rv = res[m]; res = (res - rv.min()) / (rv.max() - rv.min() + 1e-6); res[~m] = 0
depth_al = cv2.GaussianBlur(res, (0, 0), 3)
Image.fromarray((depth_al * 255).astype(np.uint8)).save(os.path.join(OUT, "_depth_detrended.png"))

A = (alpha.astype(np.float32) / 255.0)[..., None]
gy, gx = np.mgrid[0:th, 0:tw].astype(np.float32)
for ang in ANGLES:
    shift = AMP * tw * (depth_al - 0.5) * math.tan(math.radians(ang))
    mapx = (gx - shift).astype(np.float32); mapy = gy
    wrgb = cv2.remap(rgb.astype(np.float32), mapx, mapy, cv2.INTER_LINEAR, borderValue=(255, 255, 255))
    wa = cv2.remap(A[..., 0], mapx, mapy, cv2.INTER_LINEAR, borderValue=0)[..., None]
    comp = wrgb * wa + 255.0 * (1 - wa)
    Image.fromarray(comp.clip(0, 255).astype(np.uint8)).save(os.path.join(OUT, f"warp_{int(round(ang)):+04d}.png"))
print("detrend done; plane coef", [round(float(c), 5) for c in coef])
