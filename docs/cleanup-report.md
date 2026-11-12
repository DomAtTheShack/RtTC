# Cleanup report

Bigger improvements found during the cleanup on `cli-connected-mode` (2026-11-12), not done
yet. The cleanup itself removed `RC1`, merged duplicate code, removed unused code, and
brought the docs up to date.

## To do

1. **300 baud is HF.** The suite is VHF/UHF only, but 300 baud (AFSK 1600/1800 Hz) is
   still offered: in Settings > Packet speed, the HF tuning tones, the `packet_300.wav`
   test file, and the `fsk` and modem code. Remove it.
2. **`connected.enabled` is now a mute switch.** With `RC1` gone, setting it to `false`
   makes the connector do nothing. Either remove it, or document it as an "offline" mode.
3. **Version in three places.** `"0.1.1"` is typed by hand in
   `src/Connector/radio_connector.cpp` and in the docs' examples. Put a `VERSION` in
   `project()` in CMakeLists.txt and pass it to the code as a define.
4. **Windows toolchain.** CLion 2026.3's bundled MinGW can't link anything with `-static`
   (its libstdc++ is built for msvcrt, its winpthread for UCRT). Switch CLion to MSYS2
   UCRT64, which CI uses:
   `pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja`,
   then add `C:\msys64\ucrt64` in Settings > Toolchains.
5. **The CLI's connected chat has no tests.** `ConnectedChat` (commands, range test, echo)
   is in an anonymous namespace in `src/session.cpp`. Move it to its own file so
   `RadioTests` can cover it, and add an air test that runs two copies against each other.
6. **Large files.**
   - `src/main.cpp` (~1,200 lines): split into the settings menu, test tools and start.
   - `src/tests/radio_tests.cpp` (~2,350 lines): split by test group.
7. **Shared code between the two programs.**
   - The event queue, and "start direwolf and wait for its port", are written twice: in
     `src/session.cpp` and in `src/Connector/radio_connector.cpp`.
   - `Settings` mixes CLI-only fields (`sendTo`, `frequency`), connector-only fields
     (`path`, `mainAddress`, `connected`) and shared ones.
8. **One field, two meanings.** "Send to" (Duplex) and "Connect to" (Connected) are the same
   saved value (`send_to`), so switching mode carries the callsign over. Give each its own.
