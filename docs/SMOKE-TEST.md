# Hardware smoke test (CEL-1718)

Needs a PS4 or PS5 and an iOS/iPadOS device. Build per docs/BUILD-QT6.md.
Record pass/fail, device, iOS version, console, firmware.

- [ ] App launches, no crash (device and arm64 simulator)
- [ ] Local network permission prompt appears; allowing it works
- [ ] Add console manually by IP (manual host)
- [ ] Register with PSN account ID + PIN (PS4 and/or PS5)
- [ ] With discovery disabled, the manual console changes from Register to Connect after successful registration and still shows Connect after relaunch
- [ ] An older manual console that still shows Register can be linked to its saved credentials through Edit and Registered Console without entering another PIN
- [ ] Stream starts; video renders via VideoToolbox (no green/black frames)
- [ ] SDL audio plays, no crackle or drift over 5 min
- [ ] Controller (MFi/DualShock/DualSense) input works in game
- [ ] Touch/on-screen input if applicable
- [ ] Disconnect and reconnect to the same console
- [ ] Delete registered console does not hang (19620cb)
- [ ] Discovery: still broken without multicast entitlement? (README note)

README recheck: after results, update the discovery/entitlement note in README.md.
