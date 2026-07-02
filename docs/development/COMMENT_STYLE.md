# Comment Style

House rules for comments across the engine, editor, shaders, and build scripts. The goal is comments a senior engine programmer would write: they explain the non-obvious *why*, and they stay out of the way otherwise.

## Voice

- Explain intent, invariants, hidden constraints, and cross-cutting hazards. Skip anything that restates what well-named code already says.
- Impersonal and declarative. No "we", "our", "you", no asides or jokes.
- One good line beats a paragraph. Put the rationale once at the owning site; let the other call sites stay terse or point back to it.
- Wrap around 115 columns. Do not fold a line early to hit a narrow column, and do not fold function calls or string literals to fit one. This applies to this document too: prose flows one line per paragraph and reflows in the editor.

## Plain ASCII

Comment and log text is ASCII. This keeps every editor, terminal, and diff tool honest and rules out the mojibake that double-encoded bytes produce.

- No em dashes. Use a colon, semicolon, comma, or parentheses by sense.
- No decorative Unicode: arrows (`->`, not the glyph), box-drawing, ellipsis characters, multiplication/division/dot signs, Greek letters, superscripts. Write `x`, `*`, `/`, `pi`, `sigma`, `^2`, `<=`, `!=` instead.
- Exception: diacritics in an author's name inside a real citation stay (for example `Wachter & Binder` keeps its umlaut). Correct spelling of a cited name beats ASCII purity.
- UI strings are not comments. Intentional glyphs in editor UI (for example a middle-dot stat separator in an ImGui line) are a visual-design choice and are left alone.

## No project bookkeeping in comments

Comments describe the code, not the process that produced it.

- No roadmap or version labels: no `Phase B.3`, `Pillar X`, `rt-renderer C.5`, `since vX.Y`, `as of <date>`, `epic:`, or bare issue numbers like `#154`. Describe the subsystem or behavior itself instead.
- Ordered algorithm phases are fine (`Phase 1 / Phase 2` of a two-phase execute, a numbered pipeline of GPU passes). Those document the code, not the roadmap.
- An upstream tool-bug reference is fine when it is load-bearing (for example `slang#10525` next to the workaround it explains).

## Banners

One canonical form for a section banner:

```
// ---- Section name ----
```

No box-drawing rules, no `====`, no three-line blocks. A banner does not count against the one-line-beats-a-block preference, but prefer none at all.

## Blocks

A multi-line block is justified only when it carries something a single line cannot: an `invariant:` note, a hazard the code cannot show, or a pointer such as `see arch/<area>.md`. Otherwise tighten it to a line. Never restate an adjacent well-named call; delete the label instead.

## Doxygen

No `///` or `/** */` Doxygen and no `@param` / `@return` / `@brief`. Plain `//` and `/* */` only.
