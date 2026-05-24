# Security policy

NocturNation is an open-source, hobbyist crowd-lighting project maintained by a single developer. It is not a certified medical, broadcast, or safety-critical device (see the [architecture spec's safety section](https://github.com/ratcliffej/nocturnation-docs/blob/main/architecture.md#15-safety-considerations)). The measures here are best-effort.

## Reporting a vulnerability

Please report security issues **privately - do not open a public issue**.

Use GitHub's **"Report a vulnerability"** button under this repository's **Security** tab (Security → Advisories). That opens a private advisory visible only to the maintainer.

Expect a best-effort acknowledgement. There is no formal SLA - this is a single-maintainer hobby project. Coordinated disclosure is appreciated: please give us a chance to fix an issue before disclosing it publicly.

## Scope

**In scope:**

- The firmware in this repository (HAL/DAL, audio analyser, ESP-NOW transport, plug-ins).
- The PixMob IR encoder (`include/pixmob_protocol.h`) and ESP-NOW frame parsing/encoding.
- The build and release path (PlatformIO configuration, board definitions).

**Out of scope:**

- Third-party dependencies and toolchains (M5Unified, arduinoFFT, esp-dsp, PlatformIO) - report those to their own upstreams.
- PixMob bracelet hardware and the upstream reverse-engineering work it builds on.
- **By-design properties of the open protocol.** At Tier 0 (the default), NocturNation broadcasts *unauthenticated* ESP-NOW frames - anyone in radio range can transmit. A "spoofed show on an open channel" is a documented design trade-off, not a vulnerability; see the [protocol manual](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/protocol-manual.md) and [architecture spec §16](https://github.com/ratcliffej/nocturnation-docs/blob/main/architecture.md#16-security-model-overview). The channel-11 Performance-mode protections (source_id partitioning + Trust-On-First-Use) are best-effort, not cryptographic.

## Supported versions

`main` is the supported line. There are no long-term-support branches; fixes land on `main`.
