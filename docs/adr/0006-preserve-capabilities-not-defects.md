# Preserve capabilities, not baseline defects

The userver extraction baseline is an inventory and differential-test oracle,
not a requirement to reproduce defects. Ulog preserves intended logging
capabilities and records every deliberate behavioral difference, while known
bugs, contradictory defaults, unsafe error handling, and platform parsing
failures are redesigned and covered by the new contract.
