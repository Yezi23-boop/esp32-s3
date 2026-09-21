# Root AGENTS Template For ESP-IDF / ESP32

Use this as a short repository-level template. Replace bracketed placeholders and delete sections that do not apply.

> **Existing repo with detailed AGENTS.md**: If the repository already has a substantial `AGENTS.md` (100+ lines with repo-specific rules, toolchain paths, coding standards, etc.), do NOT replace it with this template. Instead, restructure the existing content to separate concerns: keep stable rules in root, move detailed norms to `docs/context/knowledge/`, and ensure the root file stays under ~100 lines. The template below is for greenfield repos or repos with trivially short AGENTS.md files.

```md
# AGENTS.md

## Project
- This is an `ESP-IDF` project targeting `[ESP32 / ESP32-S3 / ESP32-C3 / ESP32-P4]`.
- Current product goal: `[one sentence]`.
- Build system: `ESP-IDF + CMake + idf.py`.

## Read First
- `README.md`
- `sdkconfig.defaults`
- `.codex/config.toml`
- `main/`
- `components/`
- `boards/` if present
- `docs/context/` if present
- current directory and parent-directory `AGENTS.md` files

If the repository has a mature `docs/context` entrypoint such as `docs/context/INDEX.agent.md` and `docs/context/knowledge/project/project-profile.md`, prefer those over bulk-reading `README.md`, `knowledge-map.md`, or `repo-overview.md`.

## Working Rules
- Read relevant files first, then make the smallest useful change.
- Prefer reusing existing components, drivers, and initialization flow.
- Do not change unrelated files.
- Do not casually upgrade `ESP-IDF`, managed components, or toolchain versions.

## ESP-IDF Rules
- Treat `sdkconfig.defaults` as the project configuration baseline.
- Do not make large unexplained edits to generated `sdkconfig`.
- Explain `menuconfig` or Kconfig impact when configuration changes are proposed.
- Prefer new reusable modules under `components/`.
- Prefer board-local rules and entrypoints under `boards/`.

## Hardware And Safety
- Confirm pins, init order, clocks or bandwidth, and recovery paths before changing hardware-facing code.
- Do not remove reset, delay, or power-on sequence logic without evidence.
- Call out risk explicitly when touching boot, partitions, OTA, NVS, or connectivity init flow.

## Validation
- Prefer the minimum validation that matches the change.
- For document or config-only changes, re-read files and check references.
- If `scripts/context/validate_context.py` exists, use `--level light` for ordinary lookup, `--level standard` for context-doc changes, `--level routing` for entrypoint changes, and `--level full` for context tooling changes.
- For firmware changes, confirm the ESP-IDF environment first, then use `idf.py build`.
- For board verification, prefer app-only flash and bounded monitor capture for normal app changes; do not default to full `flash monitor` unless bootloader, partition, flash layout, or non-app partitions changed.
- If hardware verification was not performed, say `未实机验证` or the local equivalent.

## Local Overrides
- Use directory-level `AGENTS.md` files only for narrower local rules.
- Keep local rules consistent with the root file.
- Use `docs/context` for long-term reusable knowledge instead of growing the root file into a handbook.

## Output
- Explain what changed, why it changed, risks, and how to validate.
```
