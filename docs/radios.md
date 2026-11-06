# Radios

Which radios RtTC has been tested with, which should work, and which setups won't. A
radio only moves to **Tested** once it has sent and received packets with RtTC on the
air. If you try one, please report how it went.

## Tested

| Radio | Hookup | OS | What was checked |
| --- | --- | --- | --- |
| BTech UV-Pro | Bluetooth audio, VOX | Linux | Transmit and receive on the air ([bluetooth-audio.md](bluetooth-audio.md#what-has-been-checked)) |

CAT control has only been tested against Hamlib's dummy rig (the `cat` tests in
`RadioTests`), not a real radio yet.

## Should work

Not tested yet, but nothing in RtTC should stop them.

### Sound card interfaces

direwolf is the modem, so a radio only needs audio in and out (a USB sound card
interface like a Digirig, or a radio's built-in USB audio) and a way to key it:

- **VOX**: the radio or interface keys on audio.
- **RTS or DTR**: a serial port line, from the interface or the radio's own USB serial
  port. Some radios show two USB serial ports; the second can key while the first does
  CAT.
- **CAT**: a command through Hamlib (below).
- **AIOC cable**: keys through its own serial port (below).

### AIOC cable

The [AIOC](https://github.com/skuep/AIOC) (All-In-One-Cable) plugs into handhelds with a
Kenwood-style two-pin K plug, such as Baofengs, and into the computer's USB. It's a sound
card and a serial port in one, and keys the radio while the port's DTR line is on and
RTS is off.

1. Plug in the cable and the radio. Turn the radio's own VOX off.
2. In RadioConnectorCli, **Settings > PTT > AIOC cable**. It finds the cable's serial port
   and sound card and shows them. It finds them again each time it starts, so a new COM
   number or sound card number after re-plugging doesn't matter.
3. Use **Test tools > Tuning tone** to set the transmit level, then try a test message.

For RadioConnector, set `ptt=aioc` in `connector.ini`, or send
`{"ptt":{"type":"aioc"}}`. If the cable isn't plugged in, the radio goes to `error` and
the connector tries again every 10 s.

Use 300 or 1200 baud: the K plug is the radio's mic and speaker, so 9600 won't work (see
[Won't work](#wont-work)).

On Linux, the serial port (`/dev/ttyACM0`) needs your user in the `dialout` group (`uucp`
on Arch). The cable's firmware has to key on DTR/RTS, which is how it ships.

Not tested with a cable yet. Finding a serial port by its USB ID was checked on Linux
with another USB serial device; the Windows and macOS code for it hasn't been run.

### Radios with CAT

The radios in the Settings list are picked by name; any other Hamlib model works by its
number, or through `rigctld`.

| Maker | Models |
| --- | --- |
| Icom | IC-7300, IC-7300MK2, IC-705, IC-7610, IC-9700, IC-7100, IC-7000, IC-9100 |
| Yaesu | FTDX-3000, FTDX-10, FTDX-101D, FTDX-5000, FT-991/991A, FT-710, FT-891, FT-817, FT-818, FT-857, FT-897 |
| Kenwood | TS-590SG, TS-590S, TS-890S, TS-2000, TS-480 |
| Elecraft | K3/K3S, KX3/KX2, K4 |
| Xiegu | X6100, X6200, G90 |

The IC-7300MK2 and X6200 only show up when the installed Hamlib knows them.

### Bluetooth audio radios

The UV-50Pro, Radioddity GA-5WB and DB50-B Mini, and Vero VR-N75, VR-N76, VR-N7500 and
VR-N7600 use the same Bluetooth design as the UV-Pro. Linux is the only OS where it has
run on a radio; Windows should work but hasn't been tried
([bluetooth-audio.md](bluetooth-audio.md)).

## Won't work

| Setup | Why | Instead |
| --- | --- | --- |
| 9600 baud through the mic and speaker jacks | 9600 baud needs flat audio, straight to and from the modulator and discriminator. Mic and speaker audio is filtered, so 9600 packets don't survive | Use the radio's data port (often a 6-pin mini-DIN marked 9600), or 300/1200 baud |
| Bluetooth audio on macOS (for now) | On macOS the Bluetooth bridge's sound cards are PulseAudio devices, and direwolf only sees CoreAudio devices, so it can't reach them. The setting is hidden on macOS | A sound card interface, or Linux |
