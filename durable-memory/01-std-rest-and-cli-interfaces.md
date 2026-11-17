# Standard `lmcode` REST Endpoints & Stateless CLI Interfaces

## Summary

`lmvalkey` embeds `lmcode-cpp` (`src/include/dslvm.hpp`, `src/stdlib/rest.hpp`, `src/stdlib/std.hpp`) with the 226-signature `lmcode_std_v3` standard catalog:
- Supports `RestGet`, `RestPost`, `RestPut`, `RestDelete`, `RestPatch`, `RestHead`, `RestOptions`, `RestHandler`, `CliCommand`, and `CliOption`.
- Supports `rest_config(key, value)` and stateless `rest_cli(argv)` inside `_lmcode_` blocks alongside `valkey.cmd(...)` and `valkey.get(...)` / `valkey.set(...)`.
- Verified in `tests/integration-test.sh` against a live `lmvalkey` instance.
