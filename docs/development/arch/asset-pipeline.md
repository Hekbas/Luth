# Asset Pipeline — Architecture Details

## Overview

UUID-based asset system with two-stage loading (import → deserialize), async-first design, and automatic garbage collection.

## Data Flow

```
Engine Boot (Phase 1):
  FileSystem::InitEngine(engineRoot)  — sets luth/ as engine root
  AssetDatabase::InitEngine(luth/assets/) — registers engine shaders, fonts
  AssetManager::Init()

Project Load (Phase 2, after user selects a project):
  FileSystem::SetProjectRoot(projectRoot) — sets project root + assets path
  AssetDatabase::LoadProject(assets/) — crawls project assets/, assigns UUIDs via .meta
      ↓
  Importer::Import() — source → binary artifact (Library/Artifacts/{uuid}.luth)
      ↓
  AssetSerializer::Deserialize() — artifact → CPU data (AssetData*)
      ↓
  GPU Resource Creation — Texture::Create(), Model::Create(), etc. (main thread only)
      ↓
  AssetManager cache — shared_ptr<Asset> in s_Assets map
```

## Core Components

### AssetDatabase (Static Singleton)
- **`s_Assets`** — `unordered_map<UUID, AssetMetadata>` — main registry
- **`s_PathToUuid`** — reverse lookup (path → UUID)
- **`s_DirtyAssets`** — assets needing reimport (stale artifact hash)
- **`InitEngine(engineAssetsRoot)`** — Phase 1: registers engine-internal assets (shaders, fonts)
- **`LoadProject(projectAssetsRoot)`** — Phase 2: 3-phase scan of project assets (collect → register → cleanup orphaned .meta)
- **`UnloadProject()`** — removes project assets from registry, keeps engine assets intact
- Hash: XOR of file modification time + size, persisted in `Library/State.json`

### MetaFile (.asset.meta JSON sidecar)
```json
{
  "version": 1,
  "uuid": "...",
  "dependencies": ["uuid1", "uuid2"],
  "type_settings": { "generate_mipmaps": true, "wrap_mode": 0, ... }
}
```
- Created alongside each source asset
- Persists UUID across renames/moves
- `type_settings` drives importer behavior (texture filters, mipmap generation, etc.)

### AssetManager (Static Singleton)
- **`s_Assets`** — `unordered_map<UUID, shared_ptr<Asset>>` — in-memory cache
- **`s_LoadingAssets`** — `unordered_set<UUID>` — in-flight async loads
- **`s_UploadQueue`** — `vector<PendingUpload>` — data awaiting main-thread GPU creation

**Loading paths:**
| Method | Thread | Use Case |
|--------|--------|----------|
| `LoadAsync(uuid)` | Worker → main | Preferred. Non-blocking, via JobSystem |
| `LoadImmediate(uuid)` | Main | Editor-only. Blocking, for instant interaction |

**Async flow:** LoadAsync → LoadJob on worker → deserialize artifact → push to UploadQueue → Update() on main thread creates GPU resource → cache in s_Assets

**GC:** `Trim()` runs every 2 seconds. Evicts assets where `use_count == 1` and stale > 5 seconds. Scene holds shared_ptrs via `HoldAsset()` to prevent eviction of in-use assets.

### FileSystem (Static)
- **Dual-root architecture:**
  - `s_EngineRoot` / `s_EngineAssetsRoot` — engine shaders, fonts (luth/assets/)
  - `s_ProjectRoot` / `s_AssetsRoot` — user project assets (set when project is loaded)
- **`InitEngine(engineRoot)`** — Phase 1: sets engine root only
- **`SetProjectRoot(projectRoot)`** — Phase 2: sets project root, creates base directory structure
- **`HasProject()`** — returns whether a project is currently loaded
- **`ResolveAsset(relative)`** — searches project assets first, falls back to engine assets
- `ClassifyFileType(path)` — extension → AssetType mapping
- Type info: directory name, extension, UI color per asset type

## Importers

| Importer | Input | Process | Output |
|----------|-------|---------|--------|
| **TextureImporter** | .png/.jpg/.tga | stb_image → RGBA8 pixels, reads .meta settings | `[AssetHeader][TextureHeader][pixels]` |
| **ModelImporter** | .fbx/.obj/.gltf/.dae | Assimp (triangulate, normals, tangents, flip UVs), axis correction, extracts embedded textures + materials as side-effect | `[AssetHeader][ModelHeader][MaterialUUIDs][Meshes]` |
| **MaterialImporter** | .mat (JSON) | Wraps JSON in artifact | `[AssetHeader][JsonSize][JsonData]` |
| **ShaderImporter** | .vert / .frag / .comp (GLSL) | Infers stage from extension, compiles one file via shaderc → SPIR-V | `[AssetHeader(v=2)][ShaderHeader{Stage,SpirVSize}][SpirV]` |

All importers run on worker threads (no Vulkan access). GPU resource creation happens on main thread only.

## Binary Artifact Format

All artifacts start with `AssetHeader { magic='LUTH', version, type }`. Most assets use `version=1`; Model uses `version=2` (skeleton + animations); Shader uses `version=2` (single-stage — V1 paired `.vert+.frag` artifacts are rejected on load and re-imported). Type-specific headers follow with dimensions, counts, or sizes, then raw data. See `AssetSerializer.h` for exact layouts.

## Supporting Systems

### IOThread
- Dedicated OS thread for blocking file I/O
- Pre-allocated ring of 64 callback slots (no malloc in hot path)
- Request → read file → dispatch IOCallbackJob to JobSystem

### FileWatcher
- Dedicated OS thread, polls every ~1 second
- Detects file creation/modification/deletion via mtime comparison
- Callbacks trigger reimport or shader hot-reload

## Directory Structure

```
luth/                        ← engine (always available)
├── assets/                  ← engine-internal assets
│   ├── shaders/             ← PBR, shadow, post-processing shaders
│   └── fonts/               ← editor fonts + icons
└── source/                  ← engine source code

<project>/                   ← user project (loaded via ProjectLauncher)
├── <name>.luthproj          ← project definition file (JSON)
├── assets/                  ← project source files + .meta sidecars
│   ├── models/
│   ├── textures/
│   ├── materials/
│   ├── scenes/
│   └── shaders/
└── Library/
    ├── Artifacts/           ← binary .luth files keyed by UUID
    └── State.json           ← artifact hash map
```

## .luthproj File

```json
{
  "name": "MyProject",
  "version": "0.1"
}
```

Discovered via CLI arg, drag-drop, or the Project Launcher. The engine walks up from CWD to find the `luth/` engine root, but the project root is always explicitly selected.
