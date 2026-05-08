# NocturNation docs

This directory holds the project's design documentation as Markdown files. Markdown is the source of truth for editing; Notion is a synced mirror for sharing and visual browsing.

## Why this layout

- Editing docs locally with Claude Code (or any text editor) is fast and reliable; editing them through Notion via API/MCP turned out to be flaky and high-friction.
- Markdown lives next to the code it describes. A behaviour change and the spec change can land in the same PR, reviewable together.
- Diff-friendly, blame-friendly, version-controlled.
- Notion stays useful as a presentation layer, search index, and place to share with non-technical readers - it's just no longer the canonical source.

## Multi-app context

This is currently the StickC Plus2 firmware repo, but it's an ESP32-family project that will grow to cover additional host boards (M5 StickS3, generic ESP32 dev kits, etc.). A separate repo for the **Tildagon** receiver firmware is planned, and a third **umbrella** repo may emerge for cross-project docs.

Each file in this directory is tagged in its frontmatter with `status: cross-project` (for project-wide docs that will eventually move to the umbrella repo) or `status: stickcplus2-specific` / `status: esp32-specific` (for docs that stay here). When the umbrella repo exists, cross-project files migrate; the rest stay.

## File layout

```
docs/
├── README.md                                 # this file
├── architecture.md                           # cross-project architecture spec
└── epics/
    ├── epic-01-stickc-parity.md              # closed
    ├── epic-02-architecture-refactor.md      # closed
    └── epic-03-stickc-ui.md                  # closed
```

More Epics (4 onward) will land here as they become active.

## Notion sync convention

Every file in `docs/` carries YAML frontmatter with:

```yaml
---
title: ...
notion_url: https://www.notion.so/<id>
notion_id: <id>
notion_status: <Notion's status field, where applicable>
last_synced: YYYY-MM-DD
sync_direction: bidirectional
---
```

When syncing:

- **Markdown → Notion:** the Markdown file is the source of truth. After an edit, push to the Notion page identified by `notion_id`. Update `last_synced`.
- **Notion → Markdown:** if the Notion page has been edited directly (e.g. by a non-technical collaborator), pull and merge into the Markdown file. Update `last_synced`.

There is **no automated sync tool yet**. Sync is currently manual:

- For small edits: hand-apply the change in both places, or use Notion's built-in "Export → Markdown" for whole-page replacement.
- For larger edits: copy the Markdown body into Notion's editor (it accepts Markdown paste), or use the Notion API directly via curl with a personal integration token (more reliable than the MCP path).

A simple `tools/sync_notion.py` script may be added in a future commit if manual sync becomes a chore.

## Editing rules

- UK English in all prose.
- Minus signs `-` instead of em dashes `—`.
- Harvard referencing for any external citations (with verified clickable URLs).
- Diagrams as ASCII or Mermaid (renders on GitHub). Notion's box-drawing diagrams move over verbatim.
- Code blocks for technical content. Tables for structured data.
- One file per logical document. Don't split a single doc across files unless it grows past ~1500 lines.

## When to create a new file here vs. in the repo root

- `README.md` at the repo root: project introduction, quick start, contributor onboarding. The thing a stranger reads first.
- `LICENSE` at the repo root: legal.
- `REFERENCES.md` at the repo root: prior-art attribution.
- Everything else (architecture spec, design RFCs, Epic plans, decision records) goes in `docs/`.
