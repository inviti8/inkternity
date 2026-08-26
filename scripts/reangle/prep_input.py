"""Prepare a DrawingSpinUp input: RGB sketch -> 512x512 RGBA texture.png (alpha=fg).

Uses the same isnet_dis.onnx + normalization DrawingSpinUp's mv.py uses, so the
alpha we produce matches what the pipeline expects. Mattes the character off its
background, trims to the silhouette bbox, pads square with a small margin, resizes
to 512. Also writes mask.png (the alpha) alongside.

Usage:  python prep_input.py <src_rgb.png> <out_dir>   # writes out_dir/texture.png, mask.png
"""
import os, sys
import numpy as np
from PIL import Image
import onnxruntime as ort

SRC = sys.argv[1]
OUT_DIR = sys.argv[2]
ISNET = "dis_pretrained/isnet_dis.onnx"
SIZE = 512
MARGIN = 0.08          # padding around the character, fraction of its long side
LO, HI = 0.30, 0.65    # soft-alpha remap window (tighten the isnet matte)

sess = ort.InferenceSession(ISNET, providers=["CPUExecutionProvider"])
inp = sess.get_inputs()[0]
iname = inp.name
ishape = inp.shape
IH = ishape[2] if isinstance(ishape[2], int) else 1024
IW = ishape[3] if isinstance(ishape[3], int) else 1024
print(f"isnet input {iname} {ishape} -> using {IW}x{IH}")

img = Image.open(SRC).convert("RGB")
W0, H0 = img.size

# isnet forward (mean 0.5, std 1.0, CHW, batch1) -- identical to mv.remove_background
a = np.asarray(img.resize((IW, IH), Image.BILINEAR), dtype=np.float32)
a = (a / 255.0 - 0.5) / 1.0
a = np.transpose(a, (2, 0, 1))[None].astype(np.float32)
mask = sess.run(None, {iname: a})[0][0][0]
mask = (mask - mask.min()) / (mask.max() - mask.min() + 1e-8)

# back to full res, remap to a tighter soft alpha
m = np.asarray(Image.fromarray((mask * 255).astype(np.uint8)).resize((W0, H0), Image.BILINEAR),
               dtype=np.float32) / 255.0
m = np.clip((m - LO) / (HI - LO), 0.0, 1.0)
alpha = (m * 255).astype(np.uint8)

rgba = np.dstack([np.asarray(img, dtype=np.uint8), alpha])
ys, xs = np.where(alpha > 12)
if xs.size == 0:
    sys.exit("ERROR: empty matte -- isnet found no foreground")
x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
crop = Image.fromarray(rgba).crop((int(x0), int(y0), int(x1) + 1, int(y1) + 1))

cw, ch = crop.size
side = int(round(max(cw, ch) * (1 + 2 * MARGIN)))
canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
canvas.paste(crop, ((side - cw) // 2, (side - ch) // 2), crop)
final = canvas.resize((SIZE, SIZE), Image.LANCZOS)

os.makedirs(OUT_DIR, exist_ok=True)
final.save(os.path.join(OUT_DIR, "texture.png"))
Image.fromarray(np.asarray(final)[:, :, 3]).save(os.path.join(OUT_DIR, "mask.png"))
# also an on-gray preview so we can eyeball the matte quality
prev = np.asarray(final).astype(np.float32)
al = prev[:, :, 3:4] / 255.0
prev[:, :, :3] = prev[:, :, :3] * al + 200 * (1 - al)
Image.fromarray(prev.astype(np.uint8)[:, :, :3]).save(os.path.join(OUT_DIR, "preview_on_gray.png"))
print("bbox", (int(x0), int(y0), int(x1), int(y1)), "crop", (cw, ch), "-> 512  saved to", OUT_DIR)
