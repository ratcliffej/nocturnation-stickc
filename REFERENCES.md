# References

This project would not have been possible without the prior reverse-engineering work of others. The PixMob IR protocol implementation in [`include/pixmob_protocol.h`](include/pixmob_protocol.h) is a port of work originally published by:

- Weidman, D. (2022) *Hacking the PixMob infrared protocol to enable control of PixMob wristbands at home* [Online repository]. GitHub. Available at: <https://github.com/danielweidman/pixmob-ir-reverse-engineering>

- W., J. (2024) *PixMob_IR: PixMob IR Reverse Engineering* [Online repository]. GitHub. Available at: <https://github.com/jamesw343/PixMob_IR>

The companion documentation files `docs/ir_protocol.md` and `docs/operation.md` in the `jamesw343/PixMob_IR` repository are the authoritative source for the byte-level protocol structure used in NocturNation's `pixmob_protocol.h` C++ port.

For the full design rationale and additional references, see the NocturNation Architecture Specification, section 12.
