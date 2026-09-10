"""Regenerates the white-furnace QA fixtures into the in-repo samples project.

The furnace test is how raster-vs-path-tracer agreement is validated: a surface of known albedo lit
only by the environment must produce the same radiance down both paths, so any ratio between them is a
normalisation or occlusion defect rather than noise. Committed as fixtures because a reference metric is
worthless if the scene it was measured on cannot be reproduced.

Each fixture isolates ONE variable, and the two blind spots below are the reason there are five rather
than one:
  open_white  albedo 1.0, unoccluded. The baseline. Note albedo 1.0 makes an albedo-decode mismatch
              invisible (1^x == 1) and a flat plane has no interreflection, so this case alone cannot
              detect either -- hence open_grey.
  open_grey   albedo 0.5. Catches an albedo/decode disagreement the 1.0 case cannot see.
  open_metal  rough metal. Isolates the specular lobe (no diffuse). Expect BOTH paths to lose the same
              energy at roughness 1.0: single-scatter GGX has no multi-scatter compensation, so this
              measures agreement, not physical energy conservation.
  open_gloss  glossy dielectric. Mixed diffuse + a real specular lobe.
  roofed      floor fully roofed, camera between the two. Sky cannot reach the floor directly, so every
              photon has bounced: this is the interreflection/occlusion case, and the SIGN of any
              disagreement is diagnostic (raster darker = one-bounce GI deficit; raster brighter = an
              unoccluded env term leaking onto a surface that sees no sky).

Run:  python scripts/quality/make_furnace_fixtures.py
Then: scripts/quality/furnace.ps1   to score them all.
"""
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSETS = os.path.join(REPO, 'samples', 'assets')

SHADER = 'dce001da-6901-4c29-bfd5-0ba5df122c23'   # stock Lit
PLANE  = 'd9399440-9110-4ed9-af43-d9ea2f98fdd3'   # luth/assets/models/primitives/Plane.fbx
CUBE   = 'fd7f6b5d-9d15-4112-9a9f-b682bf62ca40'   # luth/assets/models/primitives/Cube.fbx

# Fixed UUIDs: the scenes reference the materials by UUID, so these must be stable across regeneration.
MATERIALS = {
    'qa_furnace_white': ('fa000000-0000-4000-8000-000000000001', 1.0, 0.0, 1.0),
    'qa_furnace_grey':  ('fa000000-0000-4000-8000-000000000004', 0.5, 0.0, 1.0),
    'qa_furnace_metal': ('fa000000-0000-4000-8000-000000000002', 1.0, 1.0, 1.0),
    'qa_furnace_gloss': ('fa000000-0000-4000-8000-000000000003', 1.0, 0.0, 0.35),
}

# name -> (floor material key, roofed)
SCENES = {
    'qa_furnace_open':   ('qa_furnace_white', False),
    'qa_furnace_grey':   ('qa_furnace_grey',  False),
    'qa_furnace_metal':  ('qa_furnace_metal', False),
    'qa_furnace_gloss':  ('qa_furnace_gloss', False),
    'qa_furnace_roofed': ('qa_furnace_white', True),
}


def write(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', newline='\n') as f:
        f.write(json.dumps(obj, indent=4 if path.endswith('.meta') else 1))
        f.write('\n')


def material(name, uuid, albedo, metal, rough):
    write(os.path.join(ASSETS, 'materials', name + '.mat'), {
        "alpha_cutoff": 0.5, "alpha_from_diffuse": 0, "blend_dst": 3, "blend_src": 2,
        "color": [albedo, albedo, albedo, 1.0], "cull_mode": 0,
        "emissive": [0.0, 0.0, 0.0, 1.0],
        "metalness": metal, "render_mode": 0, "roughness": rough,
        "shader": SHADER, "textures": [], "uniforms": None,
    })
    write(os.path.join(ASSETS, 'materials', name + '.mat.meta'), {
        "dependencies": [], "type_settings": {"blend_mode": "Opaque", "shader": "Lit"},
        "uuid": uuid, "version": 1,
    })


def scene(name, mat_uuid, roofed, tag):
    # Plane.fbx lies in its local XY facing +Z, so a -90 pitch lays it flat in world XZ facing +Y.
    # Getting this wrong renders the plane edge-on and the test silently measures the skybox instead.
    ents = [
        {"active": True, "parent": "", "tag": "Camera",
         "uuid": f"fa000000-0000-4000-9000-0000000000{tag}0",
         "transform": {"position": [0.0, 2.0, 0.0], "rotation": [-90.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]},
         "camera": {"farClip": 1000.0, "nearClip": 0.01, "orthographicFar": 1.0,
                    "orthographicNear": -1.0, "orthographicSize": 10.0,
                    "projection": 0, "verticalFOV": 45.0}},
        {"active": True, "parent": "", "tag": "Floor",
         "uuid": f"fa000000-0000-4000-9000-0000000000{tag}1",
         "transform": {"position": [0.0, 0.0, 0.0], "rotation": [-90.0, 0.0, 0.0],
                       "scale": [40.0, 40.0, 1.0]},
         "meshRenderer": {"isSkinned": False, "materialUUID": mat_uuid, "meshIndex": 0,
                          "modelUUID": PLANE}},
    ]
    if roofed:
        # Wider than the floor patch in view, so that patch sees no sky even at grazing angles.
        ents.append(
            {"active": True, "parent": "", "tag": "Roof",
             "uuid": f"fa000000-0000-4000-9000-0000000000{tag}2",
             "transform": {"position": [0.0, 6.0, 0.0], "rotation": [0.0, 0.0, 0.0],
                           "scale": [60.0, 0.5, 60.0]},
             "meshRenderer": {"isSkinned": False,
                              "materialUUID": MATERIALS['qa_furnace_white'][0],
                              "meshIndex": 0, "modelUUID": CUBE}})

    write(os.path.join(ASSETS, 'scenes', name + '.luth'), {"version": 1, "entities": ents})
    write(os.path.join(ASSETS, 'scenes', name + '.luth.meta'), {
        "dependencies": [], "type_settings": {},
        "uuid": f"fa000000-0000-4000-9000-0000000000f{tag}", "version": 1,
    })


for name, (uuid, albedo, metal, rough) in MATERIALS.items():
    material(name, uuid, albedo, metal, rough)
    print('material', name)

for i, (name, (mat_key, roofed)) in enumerate(SCENES.items()):
    scene(name, MATERIALS[mat_key][0], roofed, str(i))
    print('scene   ', name, '(roofed)' if roofed else '')
