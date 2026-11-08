# Security policy

## Supported versions

I review security reports against the latest commit on `main`. This is a hobby project, not a safety-certified vehicle controller.

## Report a vulnerability privately

Use [Report a vulnerability](https://github.com/tnsozcan/ducato-2023-can-dashboard/security/advisories/new). Private vulnerability reporting is enabled for this repository.

Include the affected commit, component, impact, prerequisites and minimal reproduction steps. Remove credentials, VINs and unrelated personal data. Do not post a working exploit or sensitive information in a public issue. I cannot promise a response deadline or a bug bounty.

Ordinary installation problems, calibration differences and unconfirmed CAN meanings belong in public issues after removing private data. Report unsafe wiring guidance as a bug; for an exploitable software issue use the private channel.

## Scope and deployment

Relevant components include the Python serial/network bridges, the game and bench firmware, the BeamNG Lua protocol and the installation instructions. Report upstream telemetry-server or library vulnerabilities to their respective maintainers as well.

Keep BeamNG UDP bound to localhost (127.0.0.1:4568) and the ETS2 bridge pointed at a local telemetry service. Do not expose either service to the internet. Third-party telemetry servers may have their own network/firewall behavior; follow their security guidance.

Use this project with a detached bench cluster only. `Gosterge_Bench` can send arbitrary frames and scan IDs; never connect it to a vehicle. Verify power and connector pins before applying voltage. An unlit warning lamp in a simulator is not evidence that a real vehicle system is healthy.
