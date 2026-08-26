"""Headless orbit render of a vertex-colored mesh via pytorch3d (pure CUDA, no GL/X).

FLAT/unlit: AmbientLights only, so the baked linework vertex-colors show through
with no added 3D shading -- style preserved. White background (drawing paper).

Usage: python render_p3d.py <mesh.obj> <out_dir> [angles_csv] [elev] [azim_offset]
"""
import sys, os, math
import numpy as np
import torch
import trimesh
from PIL import Image
from pytorch3d.structures import Meshes
from pytorch3d.renderer import (
    FoVPerspectiveCameras, RasterizationSettings, MeshRenderer, MeshRasterizer,
    SoftPhongShader, AmbientLights, TexturesVertex, look_at_view_transform, BlendParams,
)

MESH = sys.argv[1]
OUTDIR = sys.argv[2]
ANGLES = [float(a) for a in sys.argv[3].split(",")] if len(sys.argv) > 3 else [-30,-20,-10,0,10,20,30]
ELEV = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
AZOFF = float(sys.argv[5]) if len(sys.argv) > 5 else 0.0
DIST = float(sys.argv[6]) if len(sys.argv) > 6 else 3.0
SIZE = 768
FOV = 40.0
dev = torch.device("cuda:0")
os.makedirs(OUTDIR, exist_ok=True)

tm = trimesh.load(MESH, process=False)
if isinstance(tm, trimesh.Scene):
    tm = trimesh.util.concatenate([g for g in tm.geometry.values()])
V = np.asarray(tm.vertices, dtype=np.float32)
Fc = np.asarray(tm.faces, dtype=np.int64)
try:
    vc = np.asarray(tm.visual.vertex_colors, dtype=np.float32)[:, :3] / 255.0
except Exception:
    vc = np.ones_like(V) * 0.6
print("verts", V.shape, "faces", Fc.shape, "axis ranges  x",
      (round(V[:,0].min(),2), round(V[:,0].max(),2)), " y",
      (round(V[:,1].min(),2), round(V[:,1].max(),2)), " z",
      (round(V[:,2].min(),2), round(V[:,2].max(),2)))

# center + unit-normalize
c = (V.max(0) + V.min(0)) / 2.0
V = V - c
V = V / float(np.max(np.abs(V)))

verts = torch.tensor(V, device=dev)
faces = torch.tensor(Fc, device=dev)
tex = TexturesVertex(verts_features=torch.tensor(vc, device=dev)[None])
mesh = Meshes(verts=[verts], faces=[faces], textures=tex)

lights = AmbientLights(device=dev)  # unlit: color = vertex color
raster = RasterizationSettings(image_size=SIZE, blur_radius=0.0, faces_per_pixel=1)
blend = BlendParams(background_color=(1.0, 1.0, 1.0))

for ang in ANGLES:
    R, T = look_at_view_transform(dist=DIST, elev=ELEV, azim=ang + AZOFF)
    cam = FoVPerspectiveCameras(device=dev, R=R, T=T, fov=FOV)
    renderer = MeshRenderer(
        rasterizer=MeshRasterizer(cameras=cam, raster_settings=raster),
        shader=SoftPhongShader(device=dev, cameras=cam, lights=lights, blend_params=blend),
    )
    img = renderer(mesh)[0, ..., :3].clamp(0, 1).cpu().numpy()
    Image.fromarray((img * 255).astype(np.uint8)).save(os.path.join(OUTDIR, f"view_{int(round(ang)):+04d}.png"))

print("rendered", ANGLES, "elev", ELEV, "azoff", AZOFF, "->", OUTDIR)
