# Remote Heated T-Cal contract tests

This `native_sim` ztest locks the experimental remote Heated T-Cal wire and
transport contract without linking the Tracker's radio or sensor hardware.
The ESB implementation is currently embedded in `src/connection/esb.c` and
cannot be linked into a host test without bringing in the complete Nordic
radio stack.

The model in `src/remote_heated_tcal_model.c` is deliberately test-only. It
covers:

- complete 13-byte command PONG and result/capability PING layouts, including
  CRC-8/CCITT;
- big-endian transaction IDs and signed centi-degree targets;
- private magic/version rejection and result-marker validation;
- START confirmation/freshness/deadline boundaries;
- duplicate, conflicting-payload, result replay, and NORMAL handshake rules;
- non-zero 16-bit transaction increment/wrap semantics.

These tests lock the protocol contract. Board builds and hardware tests remain
responsible for proving that the production ESB, sensor mailbox, heater, and
calibration-owner integrations obey it.
