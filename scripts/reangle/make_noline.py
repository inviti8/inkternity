"""Approximate DrawingSpinUp's Step-1 contour removal: detect the dark outline
strokes inside the figure and inpaint them away -> a flat, line-free character.
Tests whether the drawn lines (not the silhouette) are what confuse Wonder3D/NeuS.

Usage: python make_noline.py <src_texture.png> <dst_dir>
"""
import sys, os
import numpy as np
import cv2
from PIL import Image

SRC = sys.argv[1]; DST = sys.argv[2]
os.makedirs(DST, exist_ok=True)

im = Image.open(SRC).convert("RGBA")
arr = np.asarray(im)
rgb = arr[:, :, :3].copy()
alpha = arr[:, :, 3]
gray = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY)

# line mask: dark pixels INSIDE the figure (below an adaptive-ish threshold)
inside = alpha > 30
thr = np.percentile(gray[inside], 22) if inside.any() else 90   # darkest ~22% = strokes
line_mask = ((gray < thr) & inside).astype(np.uint8) * 255
line_mask = cv2.dilate(line_mask, np.ones((3, 3), np.uint8), iterations=1)

# inpaint the strokes away using surrounding fill
flat = cv2.inpaint(rgb, line_mask, 4, cv2.INPAINT_TELEA)
# gentle smooth so residual line ghosts don't read as creases
flat = cv2.bilateralFilter(flat, 7, 40, 40)

out = np.dstack([flat, alpha])
Image.fromarray(out).save(os.path.join(DST, "texture.png"))
# preview on white
p = out.astype(np.float32); al = p[:, :, 3:4] / 255.0
p[:, :, :3] = p[:, :, :3] * al + 255 * (1 - al)
Image.fromarray(p.astype(np.uint8)[:, :, :3]).save(os.path.join(DST, "preview_on_white.png"))
Image.fromarray(line_mask).save(os.path.join(DST, "line_mask.png"))
print("noline made; thr", round(float(thr), 1), "line px", int((line_mask > 0).sum()))
