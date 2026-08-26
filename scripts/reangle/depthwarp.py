"""Depth-warp reangle (DIBR): use the mesh only as a DEPTH proxy, and parallax-warp
the artist's ORIGINAL drawing by that depth for small camera angles. Every output
pixel is the real linework -> style preserved exactly.

Steps: render mesh front ortho depth -> align it to the original texture by silhouette
bbox -> inverse-warp the original horizontally by depth*tan(angle). Inverse warp = no
holes, slight edge smear (fine at small angles).

Usage: python depthwarp.py <mesh.obj> <texture.png> <out_dir> [angles] [amp] [depthflip]
"""
import sys, os, math
import numpy as np
import torch, trimesh, cv2
from PIL import Image
from pytorch3d.structures import Meshes
from pytorch3d.renderer import (FoVOrthographicCameras, RasterizationSettings,
                                MeshRasterizer, look_at_view_transform)

MESH = sys.argv[1]; TEX = sys.argv[2]; OUT = sys.argv[3]
ANGLES = [float(a) for a in sys.argv[4].split(",")] if len(sys.argv) > 4 else [-15,-10,-5,0,5,10,15]
AMP = float(sys.argv[5]) if len(sys.argv) > 5 else 0.10
DFLIP = int(sys.argv[6]) if len(sys.argv) > 6 else 0
dev = "cuda:0"
os.makedirs(OUT, exist_ok=True)

# mesh -> front ortho depth
tm = trimesh.load(MESH, process=False)
V = np.asarray(tm.vertices, np.float32); Fc = np.asarray(tm.faces, np.int64)
c = (V.max(0) + V.min(0)) / 2; V = (V - c) / np.max(np.abs(V))
mesh = Meshes(verts=[torch.tensor(V, device=dev)], faces=[torch.tensor(Fc, device=dev)])
R, T = look_at_view_transform(dist=3.0, elev=0, azim=0)
cam = FoVOrthographicCameras(device=dev, R=R, T=T, scale_xyz=((0.9, 0.9, 0.9),))
S = 1024
frag = MeshRasterizer(cameras=cam, raster_settings=RasterizationSettings(
    image_size=S, blur_radius=0.0, faces_per_pixel=1))(mesh)
zbuf = frag.zbuf[0, ..., 0].cpu().numpy()
mask = (zbuf > -1).astype(np.uint8)
d = zbuf.copy(); dv = d[mask > 0]
depth = np.zeros_like(d); depth[mask > 0] = (dv - dv.min()) / (dv.max() - dv.min() + 1e-6)
if not DFLIP:
    depth = 1.0 - depth       # make 1 = nearest to camera
depth[mask == 0] = 0

# align depth to the texture by silhouette bbox
tex = np.asarray(Image.open(TEX).convert("RGBA")); th, tw = tex.shape[:2]
alpha = tex[:, :, 3]
ay, ax = np.where(alpha > 20); tb = (ax.min(), ay.min(), ax.max(), ay.max())
my, mx = np.where(mask > 0);  mb = (mx.min(), my.min(), mx.max(), my.max())
def place(src, box, tbox, shape, interp):
    x0, y0, x1, y1 = box; tx0, ty0, tx1, ty1 = tbox
    rs = cv2.resize(src[y0:y1+1, x0:x1+1], (tx1-tx0+1, ty1-ty0+1), interpolation=interp)
    cv = np.zeros(shape, src.dtype); cv[ty0:ty1+1, tx0:tx1+1] = rs; return cv
depth_al = place(depth.astype(np.float32), mb, tb, (th, tw), cv2.INTER_LINEAR)
depth_al = cv2.GaussianBlur(depth_al, (0, 0), 3)   # smooth the proxy depth

# inverse-warp the original by parallax
rgb = tex[:, :, :3].astype(np.float32)
A = (alpha.astype(np.float32) / 255.0)[..., None]
gy, gx = np.mgrid[0:th, 0:tw].astype(np.float32)
for ang in ANGLES:
    shift = AMP * tw * (depth_al - 0.5) * math.tan(math.radians(ang))
    mapx = (gx - shift).astype(np.float32); mapy = gy
    wrgb = cv2.remap(rgb, mapx, mapy, cv2.INTER_LINEAR, borderValue=(255, 255, 255))
    wa = cv2.remap(A[..., 0], mapx, mapy, cv2.INTER_LINEAR, borderValue=0)[..., None]
    comp = wrgb * wa + 255.0 * (1 - wa)
    Image.fromarray(comp.clip(0, 255).astype(np.uint8)).save(
        os.path.join(OUT, f"warp_{int(round(ang)):+04d}.png"))
print("done", ANGLES, "amp", AMP, "dflip", DFLIP, "-> ", OUT)
