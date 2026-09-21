---
name: esp-idf-project-context
description: "用于设计、评审、维护或迁移 ESP-IDF / ESP32 仓库的 Codex 项目上下文系统；当任务涉及 AGENTS.md、docs/context、validate_context.py 路由、project-framework、runtime-owner-contract、layering-boundary-map、memory/runs/garden policy、上下文迁移或成熟上下文库修复时使用。仅在用户要求创建、修改、修复或迁移上下文机制时使用；仅阅读上下文或修改固件时不要触发。"
---

# ESP-IDF Project Context

## Core Rule

For ESP-IDF / ESP32 repositories, maintain the context system as a low-token routing and memory layer. Do not confuse context cleanup with firmware refactors, and do not infer the product framework from source code when active context cards already define owners and boundaries.

When the user asks to update or repair the context system, implement the smallest context-only change and validate it. When the user only asks for analysis, stay diagnostic.

## Fit Check

Use this skill only when the repo has concrete ESP-IDF / ESP32 signals such as:

- `CMakeLists.txt`
- `main/`
- `sdkconfig` or `sdkconfig.defaults`
- `idf_component.yml`
- `idf.py` in docs or scripts

If the repo is not clearly ESP-IDF / ESP32, say so and use a generic context-bootstrap workflow instead.

## Default Entry

For a mature repo like this one, start with:

```powershell
uv run python scripts/context/validate_context.py --level light --q "<task keywords / file / error / route>" --brief
```

Then read only the matched source Markdown needed for evidence. Do not bulk-open `README.md`, `knowledge-map.md`, `repo-overview.md`, or `docs/context/knowledge/**`.

Treat `query.py` and `pack_context.py` as implementation details or advanced debugging tools. `validate_context.py --level light --brief` is the normal entry.

## Current Mature Framework

For this repo's framework style, preserve this authority chain:

- `docs/context/INDEX.agent.md`: agent first-read route, hard rules, domain routing, memory write table, validation levels.
- `docs/context/knowledge/project/project-profile.md`: low-token repo snapshot, startup path, current true owners, retired paths.
- `docs/context/knowledge/project/project-framework.md`: overall project framework map, startup stages, owner/resource budget rules, update triggers.
- `docs/context/knowledge/project/runtime-owner-contract.md`: startup phases, FreeRTOS owner snapshot contract, long-running services, forbidden runtime paths.
- `docs/context/knowledge/project/layering-boundary-map.md`: `App/UI -> Service -> Manager/Domain -> Driver Adapter -> Vendor/SDK` boundary and owner map.
- `docs/context/knowledge/project/context-memory-policy.md`: what is worth preserving as long-term memory.
- `docs/context/procedures/context-garden-policy.md`: stale/promotion/archive handling and garden candidate policy.

More specific active cards beat broad cards in their domain. If source code and an active card contradict each other, state the contradiction explicitly; do not silently pick one as truth.

## Planning And Framework Tasks

Trigger words include: `规划`, `框架`, `整体方案`, `路线`, `架构`, `先搭起来`, `按之前方案`, `上下文库里有`.

Workflow:

1. Run `validate_context.py --level light --q "<task keywords>" --brief`.
2. If retrieval hits `knowledge/project/*.md`, `plans/**/*.md`, or `runs/**/*.md`, open those docs before implementation code.
3. Summarize the matched docs' goal, owner, boundary, forbidden paths, and validation evidence.
4. Read source only after the docs cannot answer owner/layer/route.

## Maintenance Classes

Classify context-system work as:

- `missing`: required entry, card, validation script, or memory policy is absent.
- `stale-routing`: first-read route points to old scripts, retired docs, or superseded owner maps.
- `over-read-risk`: instructions encourage bulk context reading before light retrieval.
- `misaligned`: policy is stored in `.codex/config.toml`, board facts are scattered without entrypoint, or stable facts live only in runs.
- `framework-drift`: project-framework, runtime-owner-contract, layering-boundary-map, or project-profile disagree.
- `memory-drift`: major errors, route choices, or decisions are not being captured, or runs are becoming command noise.
- `garden-debt`: stale/promotion/archive/broken-owner candidates need human review.

For mature repos, repair only the smallest broken route or stale card. Do not propose a broad migration unless the existing mechanism is absent or unusable.

## Memory And Runs

Record only reusable, high-value memory:

- major error signatures: build/link failure, panic/Guru, watchdog, assertion, `ESP_ERR_*`, `NO_MEM`, serial/board anomalies, hardware communication failures, context validation failures
- route choices: tried route, rejected route, why it failed or was dropped, chosen next route, evidence
- owner/framework decisions, long-lived constraints, board evidence, verification results that future agents would otherwise repeat

Prefer:

```powershell
uv run python scripts/context/log_attempt.py --title "<short-title>" --status partial --record-because error-signature --record-because evidence --changed "<owner/path>" --tried "<command/action>" --avoid "<do-not-repeat>" --evidence "<proof>" --next "<next evidence>"
```

Use `route-choice` instead of `error-signature` when the useful memory is the path decision itself.

## Framework Update Triggers

If a change alters any of these, update `docs/context/knowledge/project/project-framework.md` and `docs/context/CHANGELOG.md`:

- startup phase order, ready gate, or UI-first-frame strategy
- long-lived owner, resource owner, call direction, or layer boundary
- FreeRTOS owner task/snapshot/queue/notification/event group contract
- resource release, blocker, budget, sleep permission, or power state semantics
- network/provisioning mainline, UI generated/custom/runtime boundary, background service/session route
- context routing, first-read rules, validation levels, memory/garden behavior

If the change is only a small implementation bugfix and does not alter owner, route, or framework semantics, do not inflate it into a framework update.

## Domain Routing

`docs/context/INDEX.agent.md` owns the current domain routing table. If a context card with `route_area` frontmatter is created or modified, update the generated domain-routing section by running:

```powershell
uv run python scripts/context/generate_index.py
```

Then validate at `routing` level.

## Validation

Use context validation levels by impact:

- `light`: normal lookup and anti-repeat check
- `standard`: context-document-only changes
- `routing`: index, entrypoint, generated domain routing, golden queries, retrieval behavior
- `full`: `scripts/context`, memory gate, promotion/archive/garden, validation mechanism

Examples:

```powershell
uv run python scripts/context/validate_context.py --level standard --q "project framework runtime owner layering context" --brief
uv run python scripts/context/validate_context.py --level routing --q "agent index domain routing query golden" --brief
uv run python scripts/context/validate_context.py --level full --q "context memory garden runs validate_context" --brief
```

Context-only changes do not require firmware build, flash, or monitor.

## Output Contract

For analysis responses, use:

1. `Project Fit`
2. `Current State`
3. `Framework / Routing Findings`
4. `Minimum Fix`
5. `Validation And Risks`

For implementation responses, summarize changed files, validation commands, and any remaining context risks.

## References

Read bundled references only when needed:

- `references/current-repo-context-rules.md`: current mature repo style rules.
- `references/migration-checklist.md`: converting or repairing an existing ESP-IDF repo.
- `references/root-agents-template.md`: proposing or rewriting a root `AGENTS.md`.
- `references/codex-config-template.toml`: `.codex/config.toml` markers or fallback docs.
- `references/board-agents-template.md`: documentation-first board layer.
- `references/context-mechanism-note-template.md`: long-term note explaining context split.
