"""Front-projection reangle: texture the mesh with the ORIGINAL drawing (front
orthographic projection), then orbit a small amount. Visible front faces show the
artist's real crisp pixels, displaced by the mesh depth => style preserved.

Usage: python render_proj.py <mesh.obj> <texture.png> <out_dir> [angles_csv] [scale] [vflip]
"""
import sys, os
import numpy as np
import torch, trimesh
from PIL import Image
from pytorch3d.structures import Meshes
from pytorch3d.renderer import (
    FoVOrthographicCameras, RasterizationSettings, MeshRenderer, MeshRasterizer,
    SoftPhongShader, AmbientLights, TexturesUV, look_at_view_transform, BlendParams,
)

MESH = sys.argv[1]; TEX = sys.argv[2]; OUTDIR = sys.argv[3]
ANGLES = [float(a) for a in sys.argv[4].split(",")] if len(sys.argv) > 4 else [-20,-10,0,10,20]
SCALE = float(sys.argv[5]) if len(sys.argv) > 5 else 0.5
VFLIP = int(sys.argv[6]) if len(sys.argv) > 6 else 0
MARGIN = 0.08
SIZE = 768
dev = torch.device("cuda:0")
os.makedirs(OUTDIR, exist_ok=True)

tm = trimesh.load(MESH, process=False)
V = np.asarray(tm.vertices, dtype=np.float32); Fc = np.asarray(tm.faces, dtype=np.int64)
c = (V.max(0) + V.min(0)) / 2.0; V = V - c; V = V / float(np.max(np.abs(V)))

# front-projection UVs: mesh xy-bbox -> the character region [lo,hi] of the texture
xmn, xmx = V[:,0].min(), V[:,0].max(); ymn, ymx = V[:,1].min(), V[:,1].max()
u = (V[:,0]-xmn)/(xmx-xmn); vv = (V[:,1]-ymn)/(ymx-ymn)
lo = MARGIN/(1+2*MARGIN); hi = (1+MARGIN)/(1+2*MARGIN)
u = lo + u*(hi-lo); vv = lo + vv*(hi-lo)
if VFLIP: vv = 1.0 - vv
uv = torch.tensor(np.stack([u, vv], 1), dtype=torch.float32, device=dev)

img = np.asarray(Image.open(TEX).convert("RGB"), dtype=np.float32) / 255.0
maps = torch.tensor(img, device=dev)[None]
verts = torch.tensor(V, device=dev); faces = torch.tensor(Fc, device=dev)
mesh = Meshes(verts=[verts], faces=[faces],
              textures=TexturesUV(maps=maps, faces_uvs=[faces], verts_uvs=[uv]))

lights = AmbientLights(device=dev)
raster = RasterizationSettings(image_size=SIZE, blur_radius=0.0, faces_per_pixel=1)
blend = BlendParams(background_color=(1.0, 1.0, 1.0))
for ang in ANGLES:
    R, T = look_at_view_transform(dist=2.5, elev=0, azim=ang)
    cam = FoVOrthographicCameras(device=dev, R=R, T=T, scale_xyz=((SCALE, SCALE, SCALE),))
    renderer = MeshRenderer(rasterizer=MeshRasterizer(cameras=cam, raster_settings=raster),
                            shader=SoftPhongShader(device=dev, cameras=cam, lights=lights, blend_params=blend))
    out = renderer(mesh)[0, ..., :3].clamp(0, 1).cpu().numpy()
    Image.fromarray((out*255).astype(np.uint8)).save(os.path.join(OUTDIR, f"proj_{int(round(ang)):+04d}.png"))
print("done", ANGLES, "scale", SCALE, "vflip", VFLIP)
