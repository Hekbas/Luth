# v2.7.1 — editor-style-assets

**Date:** 2026-04-19
**Commits:** 7 (on `epic/editor-style-assets`)
**Issue:** [#86](https://github.com/Hekbas/Luth/issues/86)

---

## Overview

Move `StylePreset` data out of source code into JSON assets. `EditorStyle.cpp`
drops from 616 → 280 LOC (−55%); the 4 built-in presets now live under
`luth/assets/styles/*.json` and the editor gains Style → Save / Load menu
entries. Fixes a standing font-merge bug in Bubblegum/Matrix along the way.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | `StylePreset` JSON (de)serialise helpers; string-type switch for `Name`/`MainFontName` | [`5b7a807`](../../../../commit/5b7a807) |
| B | Export 4 built-ins to `luth/assets/styles/{Custom,Bubblegum,Matrix,Rider}.json` | [`2b33d77`](../../../../commit/2b33d77) |
| C | Replace `Custom()/Bubblegum()/Matrix()/Rider()` factories with `LoadBuiltin(name)` | [`e22dfc7`](../../../../commit/e22dfc7) |
| D | Style menu Save/Load entries; `EditorSettings::activeStylePath` persistence | [`b4a85bd`](../../../../commit/b4a85bd) |
| E | Drop per-preset `Editor::Set*Style` API; single `Editor::LoadStyle(nameOrPath)` | [`9d0aef4`](../../../../commit/9d0aef4) |
| F | Merge FA-Solid into Bubblegum/Matrix main fonts (standing bug) | [`faab80d`](../../../../commit/faab80d) |
| G | Version bump + docs | this commit |

---

## Key Changes

- **JSON schema** — flat top-level object: `name`, `font: {mainFont, mainSize, mergeMainWithSolid, iconSize}`, scalar style fields, `colors: {}` keyed by stable `ImGui::GetStyleColorName` strings (robust against enum reordering). Obsolete aliases (`TabActive`/`TabUnfocused`/`NavHighlight`) resolve to canonical names.
- **`LoadBuiltin(name)`** reads `FileSystem::EngineAssetsPath("styles") / "<name>.json"`.
- **Persistence** — `EditorSettings::activeStylePath` takes precedence over `activeStyle` when non-empty; a style loaded via file dialog survives editor restart.
- **`Editor::LoadStyle(nameOrPath)`** is the single deferred public API — detects path vs. name by extension/separator. Menu + settings hand-off unchanged.
- **Font-merge fix (F)** — Bubblegum and Matrix previously set `mergeMainWithSolid=false` with `iconSize=48`, so `PushFont(FASolid)` resolved to a standalone 48pt glyph-only font with no text glyphs. Panel title bars that combined icon + text (e.g. `ICON_FA_LIST "  Hierarchy"`) rendered the icon but dropped the text. Now both styles merge FA-Solid at main-font size like Custom/Rider.

---

## Build Verification

- 7 atomic commits on `epic/editor-style-assets`; every commit builds Debug x64 clean.
- Runtime smoke (user-tested): all 4 built-in styles render, round-trip of Save Current As... → Load From File... works, custom path persists across restart, Bubblegum/Matrix panel titles now render icon + text.

---

## Next

`editor-widgets-reorg` — split `UI.{h,cpp}` into `widgets/{Properties,AssetSlot,CollapsingHeader,InfoTable,TexturePreview}.{h,cpp}`; delete `UI.h`/`UI.cpp`.
