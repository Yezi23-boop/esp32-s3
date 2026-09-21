# Current Repo Context Rules

Use this reference when the target repository already has a mature context garden like this one.

## Entry Points

- Read `docs/context/INDEX.agent.md` and `docs/context/knowledge/project/project-profile.md` before broad exploration.
- Do not bulk-open `README.md`, `knowledge-map.md`, `repo-overview.md`, or `docs/context/knowledge/**` by default.
- For normal tasks, start with:

```powershell
uv run python scripts/context/validate_context.py --level light --q "<task keywords>" --brief
```

## Authority Chain

- `INDEX.agent.md`: first-read route, hard rules, domain routing, memory write table, validation levels.
- `project-profile.md`: low-token repo snapshot, current true owners, retired paths.
- `project-framework.md`: overall framework, startup stages, owner/resource budget rules, update triggers.
- `runtime-owner-contract.md`: FreeRTOS owner snapshot contract, long-running services, forbidden runtime paths.
- `layering-boundary-map.md`: `App/UI -> Service -> Manager/Domain -> Driver Adapter -> Vendor/SDK`.
- `context-memory-policy.md`: what is worth preserving.
- `context-garden-policy.md`: stale/promotion/archive/broken-owner candidate handling.

More specific active cards beat broad cards in their domain. If source and an active card contradict, state the contradiction instead of silently choosing one.

## Validation Levels

- `light`: normal lookup, anti-repeat check, and brief routing.
- `standard`: context-document-only changes.
- `routing`: changes to indexes, entrypoints, retrieval baselines, or golden routing data.
- `full`: changes to `scripts/context`, memory, promotion, archive, or validation mechanisms.

## Mature-Repo Maintenance Rules

- Preserve the `INDEX.agent.md -> project-profile.md -> validate_context.py` path.
- Treat `query.py` and `pack_context.py` as advanced/debug tools, not the default workflow.
- If retrieval hits planning or architecture docs, summarize goal, owner, boundary, and forbidden paths before reading implementation code.
- Record reusable attempts, failures, board logs, and decisions in the matching context layer and update `docs/context/CHANGELOG.md` when policy requires it.
- Use `runs/` for major error signatures and route choices that future agents may repeat; use `error-signature` or `route-choice` plus `evidence`.
- If a context card with `route_area` frontmatter changes, run `uv run python scripts/context/generate_index.py`, then validate routing.
- Framework changes update both `project-framework.md` and `docs/context/CHANGELOG.md`.

## Do Not Do

- Do not replace a mature `AGENTS.md` with a generic template.
- Do not put policy prose into `.codex/config.toml`.
- Do not infer product framework directly from current source when current context cards already define owners and boundaries.
- Do not run firmware build, flash, or monitor for context-only changes.
