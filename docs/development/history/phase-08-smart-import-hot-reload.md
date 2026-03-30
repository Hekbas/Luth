# Phase 8 — Smart Import & Hot Reload

**Date:** 2026-03-31
**Scope:** Asset import quality-of-life, Project Panel hot reload, FileWatcher stability

---

## Goals

1. Stop losing textures on model import — find them automatically even when the DCC path doesn't match
2. Let the user fix any remaining missing textures via an editor dialog
3. Project Panel should reflect external file changes in real time
4. Make dropping a model file as frictionless as possible (right folder, textures included, immediate import)

---

## Part A — Smart Texture Discovery

### TextureResolver (`TextureResolver.h/.cpp`)

New standalone utility with 4 strategies tried in order (stops at first match):

1. **Direct** — exact Assimp-reported path relative to model dir
2. **Filename-only** — strip any subdirectory prefix; look for bare filename in model dir
3. **Sibling dirs** — try `textures/`, `Textures/`, `tex/`, `Tex/`, `maps/`, `Maps/`, `images/`, `Images/`, `texture/`, `Texture/`
4. **Recursive** — `recursive_directory_iterator` within model dir, depth-capped at 3 levels

When a non-direct strategy succeeds, logs which strategy resolved it.

### ImportReport (`ImportReport.h`)

```cpp
struct UnresolvedTexture {
    std::string MaterialName;
    fs::path    MaterialPath;   // Path to .mat on disk (for patching)
    std::string OriginalPath;   // Raw Assimp-reported path
    MapType     Type;
    fs::path    UserProvidedPath;
};

struct ImportReport {
    fs::path ModelPath;
    std::vector<UnresolvedTexture> Unresolved;
};
```

`ModelImporter` clears `s_LastImportReport` at the top of `Import()`, populates it during material processing, and exposes it via `GetLastImportReport()`.

### TextureRemapDialog (`TextureRemapDialog.h/.cpp`)

ImGui modal shown automatically after any model import that has unresolved textures. Features:

- Unresolved textures grouped by material, each showing type badge + original path
- **Browse** button per texture (native file dialog)
- **Search Directory** field with Browse + "Resolve All from Directory" — batch-resolves by filename matching
- **Apply** — patches `.mat` JSON on disk with resolved UUIDs, deletes stale material artifacts for reimport
- **Skip** — close without changes
- **Assets → Resolve Missing Textures...** menu item for re-opening after dismiss (greyed when nothing to resolve)

---

## Part B — Project Panel Hot Reload

### FileWatcher integration in AssetDatabase

New static members:
- `s_FileWatcher` — `unique_ptr<FileWatcher>`, 1 s polling interval
- `s_PendingChanges` — `vector<pair<path, FileStatus>>`, filled from watcher thread under `s_PendingMutex`
- `s_ChangeCallbacks` — list of `function<void()>` notified after each batch

New API:
- `StartWatching()` / `StopWatching()` — lifecycle (stop called from `Shutdown()`)
- `AddChangeCallback(cb)` — subscribe
- `ProcessPendingChanges()` — drain queue on main thread each frame:
  - **Created**: classify, create `.meta` if absent, `RegisterAsset`, mark dirty
  - **Modified**: recalculate hash, mark dirty, delete stale artifact
  - **Deleted**: `UnregisterAsset`, delete `.meta` and artifact

Skips `.meta` files and the `Library/` directory in the watcher callback.

### App main loop

```cpp
AssetManager::Update();
AssetDatabase::ProcessPendingChanges();  // ← new
Editor::Render();
```

### ProjectPanel wiring

`OnInit()` registers a callback + starts the watcher:
```cpp
AssetDatabase::AddChangeCallback([this]() { m_NeedsRefresh = true; });
AssetDatabase::StartWatching();
```

`OnRender()` checks and applies at the top of the frame.

`Refresh()` now preserves the current directory across rebuilds via `FindNodeByPath()`, falling back to root only if the directory was deleted.

---

## Part C — Drop Improvements & Bug Fixes

### OnFileDrop rewrite (`App.cpp`)

Old behaviour: hardcoded destination via `FileSystem::GetPath()` → always `assets/models/`. No eagerness.

New behaviour:
1. **Drop target = ProjectPanel's current directory** (`GetCurrentDirectory()` accessor added to ProjectPanel)
2. **Texture co-location**: for model drops, scans the source directory (and common sibling texture dirs) for image files and copies them into `{ModelName}_Textures/` next to the model — so the importer finds them immediately
3. **Eager import**: calls `AssetDatabase::RegisterAsset()` + `AssetManager::Import()` synchronously after copy, so the asset is ready without a restart

### FileWatcher TOCTOU crash fix (`FileWatcher.cpp`)

**Bug:** `fs::last_write_time(path)` (throwing overload) called on a file that was deleted between the directory scan and the timestamp read. Exception `0x80000003` crashed the watcher thread.

**Fix:**
- Wrap `recursive_directory_iterator` in `try/catch` (directory deleted mid-traversal)
- Use `fs::last_write_time(path, ec)` non-throwing overload; skip path if `ec` is set
- Same fix applied to the initial baseline scan
- Added `skip_permission_denied` option to both scan sites
- Deleted-file check uses `unordered_set` instead of `std::find` on a vector (O(1) vs O(n))

---

## Files Created

| File | Purpose |
|------|---------|
| `luth/source/luth/resources/importers/TextureResolver.h` | Resolver struct + function declaration |
| `luth/source/luth/resources/importers/TextureResolver.cpp` | 4-strategy search implementation |
| `luth/source/luth/resources/importers/ImportReport.h` | UnresolvedTexture + ImportReport structs |
| `luth/source/luth/editor/panels/TextureRemapDialog.h` | Dialog class declaration |
| `luth/source/luth/editor/panels/TextureRemapDialog.cpp` | Dialog rendering + ApplyResolutions() |

## Files Modified

| File | Changes |
|------|---------|
| `luth/source/luth/resources/importers/ModelImporter.h` | Added `GetLastImportReport()` |
| `luth/source/luth/resources/importers/ModelImporter.cpp` | Uses TextureResolver + ImportReport in ProcessMaterial; clears/populates report in Import() |
| `luth/source/luth/resources/AssetDatabase.h` | FileWatcher members, ProcessPendingChanges, AddChangeCallback, StartWatching/StopWatching |
| `luth/source/luth/resources/AssetDatabase.cpp` | Implements hot-reload pipeline; stores s_ProjectRoot |
| `luth/source/luth/resources/FileWatcher.cpp` | TOCTOU crash fix; unordered_set deleted-file check |
| `luth/source/luth/core/App.cpp` | ProcessPendingChanges in loop; rewrote OnFileDrop (current dir target, texture copy, eager import) |
| `luth/source/luth/editor/Editor.h` | Added s_ShowTextureRemapDialog |
| `luth/source/luth/editor/Editor.cpp` | Includes TextureRemapDialog; auto-open on import; Assets menu |
| `luth/source/luth/editor/panels/ProjectPanel.h` | Added m_NeedsRefresh, GetCurrentDirectory() |
| `luth/source/luth/editor/panels/ProjectPanel.cpp` | OnInit wires hot reload; Refresh() preserves dir via FindNodeByPath |
