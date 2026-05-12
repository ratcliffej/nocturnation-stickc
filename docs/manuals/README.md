---
title: "NocturNation manuals - index"
notion_url: https://www.notion.so/35ebd067740580369084dc6f9b2145e8
notion_id: 35ebd067740580369084dc6f9b2145e8
last_synced: 2026-05-12
sync_direction: bidirectional
---

# NocturNation manuals

This directory holds the public-facing manuals for the NocturNation crowd-lighting firmware. They are the canonical entry points for operators and implementers; the architecture specification stays the internal design document.

## Which manual do I want?

- **I'm setting up NocturNation at a venue.** Read the [user manual](user-manual.md). It is meant to be read top-to-bottom by a newcomer and serves as reference for an experienced operator afterwards. It covers theory of operation, hardware setup, firmware installation, the configuration menu tree, modes and shows, troubleshooting, and a glossary.
- **I'm building a new receiver or transmitter for the NocturNation protocol.** Read the [protocol manual](protocol-manual.md). It specifies the ESP-NOW wireless layer, frame formats, class+group addressing, the PixMob infra-red encoding annex, channel discovery, the firmware-side non-volatile-storage schema, and conformance requirements for a receiver.
- **I'm contributing show plug-ins to the firmware.** Read [developing-shows.md](../developing-shows.md) for the `Show` plug-in surface. The user manual covers how operators pick and configure shows at run-time; the developer guide covers how you write one.
- **I want to understand the design decisions.** Read the [architecture specification](../architecture.md). It is internal design notes, kept bidirectionally synced with the Notion source-of-truth page. The manuals here are the externally publishable distillation of it.

## Document status

| Document | Status | Audience |
|---|---|---|
| [user-manual.md](user-manual.md) | Draft | Operators |
| [protocol-manual.md](protocol-manual.md) | Draft | Implementers |
| [flow-diagrams.md](flow-diagrams.md) | Draft | Both - 8 Mermaid diagrams covering topology, boot, modes, analyser, dispatch, receive, routing, configuration |

## Conventions

- UK English throughout. Spelling: colour, behaviour, organise.
- All references use Harvard style (Author, Year) where applicable; code references use `file:line` form.
- Numeric byte values are shown in hexadecimal with the `0x` prefix. Ranges are inclusive on both ends.
- Normative language in the protocol manual follows IETF RFC 2119: MUST, SHOULD, MAY.

## Licence

The manuals are licensed under [Creative Commons Attribution-ShareAlike 4.0](https://creativecommons.org/licenses/by-sa/4.0/). The firmware code is MIT. Hardware designs (when published) are CERN-OHL-S 2.0.
