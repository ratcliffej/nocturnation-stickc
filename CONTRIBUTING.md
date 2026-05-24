# Contributing to NocturNation (StickC firmware)

Thanks for your interest. Issues and pull requests are welcome.

## Ground rules

- Be kind and constructive.
- Contributions are accepted under the repository's [MIT licence](LICENSE).
- For anything that touches the wire protocol, the [protocol manual](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/protocol-manual.md) is the source of truth. A wire change must update the manual **and** both implementations - this firmware and the [Tildagon app](https://github.com/ratcliffej/nocturnation-tildagon) - or it will not be merged.

## Pull requests

- Keep PRs focused: one logical change per PR.
- Explain *why*, not just *what*.
- **Every PR is read line-by-line before merge.** Expect heightened scrutiny for changes to:
  - `.github/` (workflows, actions, repository configuration),
  - dependencies and build config (`platformio.ini`, `boards/`, pinned library versions),
  - the IR encoder and the protocol / transport surface.
- New or changed behaviour needs tests. PRs that do not build clean or pass the test suite will not be merged.

## Behaviour-preservation invariants

Two things are deliberately locked and must not drift without explicit discussion in the PR:

- **The PixMob IR encoder** is parity-tested byte-for-byte against [jamesw343/PixMob_IR](https://github.com/jamesw343/PixMob_IR)'s Python reference. If you change it, regenerate the reference vectors **from the upstream Python encoder**, never from the local C++ output - upstream is the truth.
- **Beat-detection tuning** is bench-calibrated against real audio; justify changes against listening tests, not unit tests alone.

## Testing

```sh
pio test -e native                                   # native unit tests (plus the other native_* envs)
pio run -e m5stack-stickcs3 -e m5stack-stickcplus2   # build both targets; must compile clean
```

Warnings are treated as signal. Hardware verification (audio + IR + bracelet response) is described in the [user manual](https://github.com/ratcliffej/nocturnation-docs/blob/main/manuals/user-manual.md#5-modes-and-shows).

## Adding a Show

The Director's performance is a Show plug-in. See the [developer guide](https://github.com/ratcliffej/nocturnation-docs/blob/main/developing-shows.md) for the `Show` base class, analyser hooks, `render_fx` routing, persistence, and the testing pattern.
