"""Render a textured/vertex-colored mesh at a small camera orbit -> stills.

Style-preservation is the whole point, so we render FLAT (unlit): the baked
linework texture shows through with NO added 3D shading. Transparent background.

Usage:
  python render_orbit.py <mesh.obj> <out_dir> [angles_csv] [base_rot_xyz_deg]
  e.g. python render_orbit.py mesh/xxx.obj out "-30,-20,-10,0,10,20,30" "0,0,0"
"""
import os, sys, math
os.environ.setdefault("PYOPENGL_PLATFORM", "egl")
import numpy as np
import trimesh
from PIL import Image
import pyrender

MESH = sys.argv[1]
OUTDIR = sys.argv[2]
ANGLES = [float(a) for a in sys.argv[3].split(",")] if len(sys.argv) > 3 else [-30, -20, -10, 0, 10, 20, 30]
BROT = [float(a) for a in sys.argv[4].split(",")] if len(sys.argv) > 4 else [0, 0, 0]
W = H = 768
CAM_DIST = 2.3
YFOV = np.pi / 5.0

os.makedirs(OUTDIR, exist_ok=True)
tm = trimesh.load(MESH, process=False)
if isinstance(tm, trimesh.Scene):
    tm = trimesh.util.concatenate([g for g in tm.geometry.values()])
print("loaded:", MESH, "| verts", len(tm.vertices), "| visual", type(tm.visual).__name__,
      "| has_vertex_colors", getattr(tm.visual, "kind", None))

# optional base reorientation (to fix front axis), then center + unit-normalize
for axis, ang in zip("xyz", BROT):
    if ang:
        Rm = trimesh.transformations.rotation_matrix(math.radians(ang), {"x": [1,0,0], "y": [0,1,0], "z": [0,0,1]}[axis])
        tm.apply_transform(Rm)
tm.apply_translation(-tm.bounding_box.centroid)
tm.apply_scale(1.0 / float(np.max(tm.extents)))

mesh = pyrender.Mesh.from_trimesh(tm, smooth=False)
scene = pyrender.Scene(bg_color=[1.0, 1.0, 1.0, 0.0], ambient_light=[1, 1, 1])
scene.add(mesh)
cam = pyrender.PerspectiveCamera(yfov=YFOV)
r = pyrender.OffscreenRenderer(W, H)

for ang in ANGLES:
    a = math.radians(ang)
    Ry = np.array([[math.cos(a), 0, math.sin(a)], [0, 1, 0], [-math.sin(a), 0, math.cos(a)]])
    cpose = np.eye(4)
    cpose[:3, :3] = Ry
    cpose[:3, 3] = Ry @ np.array([0, 0, CAM_DIST])
    cnode = scene.add(cam, pose=cpose)
    color, _ = r.render(scene, flags=pyrender.RenderFlags.RGBA | pyrender.RenderFlags.FLAT)
    Image.fromarray(color).save(os.path.join(OUTDIR, f"view_{int(round(ang)):+03d}.png"))
    scene.remove_node(cnode)

r.delete()
print("rendered angles:", ANGLES, "-> ", OUTDIR)
