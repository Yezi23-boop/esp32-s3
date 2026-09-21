---
id: project-context-mechanism
tags:
  - project
  - codex
  - context
  - agents
summary: Describes how repository-level Codex context is split across AGENTS.md, .codex/config.toml, boards, and docs/context.
last_reviewed: 2026-03-30
---

# Project Context Mechanism

## Purpose

This repository uses a layered Codex project-context structure so stable rules, local overrides, and long-term knowledge do not collapse into one file.

## Layers

### Root `AGENTS.md`

Use the root file for stable repository rules:

- project identity
- read-first entrypoints
- working rules
- safety boundaries
- validation expectations

### `.codex/config.toml`

Use project config only for:

- project-root markers
- project-document fallback entrypoints

Do not store policy prose or task-specific instructions here.

### `boards/`

Use `boards/` as the board-local guidance layer:

- board entrypoints
- board-local AGENTS rules
- board-focused validation notes

Do not assume `boards/` owns all runtime board mappings unless the repository explicitly migrated code there.

### `docs/context`

Use `docs/context` for long-term reusable knowledge:

- mechanism notes
- architecture decisions
- hardware baselines
- migration notes

#### Retrieval Toolchain

If the repository has a context retrieval toolchain (e.g. `scripts/context/query.py`, `scripts/context/pack_context.py`, `scripts/context/validate_context.py`), document its usage in the root `AGENTS.md` so agents know how to query history and stable knowledge. The toolchain is the recommended entrypoint over reading raw context files directly, because it filters and ranks results by relevance.

Preferred workflow when `validate_context.py` exists:

1. Non-trivial tasks: run `uv run python scripts/context/validate_context.py --level light --q "<keywords>" --brief`.
2. Open only the highest-signal files named by the brief.
3. Treat lower-level scripts such as `query.py` and `pack_context.py` as implementation details or advanced debugging tools.
4. Write back reusable findings per the garden policy, and update `CHANGELOG.md` when required.
5. Validate context integrity with the narrowest level:
   - `light` for normal lookup and anti-repeat checks.
   - `standard` for context-document-only changes.
   - `routing` for entrypoint, index, or retrieval-baseline changes.
   - `full` for `scripts/context`, memory, promotion, or archive mechanism changes.

Do not make agents bulk-open `README.md`, `knowledge-map.md`, `repo-overview.md`, or `docs/context/knowledge/**` before the light retrieval entrypoint has indicated they are needed.

## Local Overrides

Local `AGENTS.md` files may exist under `components/` or other subdirectories.
These should narrow scope, not replace root rules.
