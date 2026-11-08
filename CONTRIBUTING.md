# Contributing

Thanks for helping decode the 2023 Ducato cluster. English and Turkish reports are welcome.

## Before opening an issue

Read the [setup guide](docs/KURULUM_TR.md), the [CAN message reference](docs/CAN_MESSAGES.md), known limits in the [README](README.md), and existing issues. Please use the bug, feature or CAN-finding template. For security vulnerabilities use the private process in [SECURITY.md](SECURITY.md).

## What to include

- Repository commit, Windows/Python/Arduino IDE versions, and game version.
- Cluster part number, MCP2515 crystal frequency, module type and wiring.
- Which bus you used: 125 kbit/s body CAN (pins 5/6) or 500 kbit/s CAN (pins 11-14).
- Expected behavior, actual behavior, steps to reproduce, and relevant serial output.
- Whether you tested on hardware, in software only, or inferred the result from another source.

Remove personal information, VINs, credentials and unrelated CAN traffic from logs. I do not need your private vehicle history or a complete unredacted capture.

## CAN findings

The most wanted findings are the coolant temperature bar, turn signals, headlights, parking brake and cruise control.

Use zero-based byte indexes b0..b7 and hex IDs/data. State the bus, ID, DLC, interval, baseline payload, exact changed bits and observed effect. Say which other frames were running: many lamps here are "module missing" timeouts, and some reactions only appear in combination. Keep source-derived information and hypotheses separate from bench observations. Link sources and preserve attribution. Do not claim a result applies to every Ducato variant.

Use an isolated, detached bench setup. `Gosterge_Bench` transmits and scans; never connect it, or any experimental transmitter, to a vehicle. Verify numbered connector cavities for your exact part number; never infer power pins from wire colors.

## Pull requests

Keep changes focused and explain the problem, resulting behavior and validation. Preserve fixed hardware settings unless a change explicitly adds a documented configuration: Nano ATmega328P, 8 MHz MCP2515, CS D10 and INT D2, 500 kbit/s for the game firmware.

In `Ducato2023_Game`, keep boot TX disabled, bounded buffers, CAN fault handling and the telemetry timeout intact. Do not add captured-drive replay or private research logs.

Run applicable checks from the repository root:

```powershell
py bridge/ets2_bridge.py --self-test
py bridge/beamng_bridge.py --self-test
```

For firmware changes, compile the changed sketch (`firmware/Ducato2023_Game` or `firmware/Gosterge_Bench`) for Arduino Nano ATmega328P with mcp_can 1.5.1 and report flash/RAM use. For Lua changes, describe how you checked vehicle reload and the affected signals. State clearly if real hardware or a game was unavailable. Documentation-only changes need link/content checks, not a new firmware build.

By contributing, you agree that your original contributions are provided under this repository's [MIT License](LICENSE). Only submit material you have permission to share; external dependencies keep their own licenses. Please follow the [Code of Conduct](CODE_OF_CONDUCT.md).
