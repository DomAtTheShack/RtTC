# Radio Connector protocol

Status: **built** (`RadioConnector`: `src/Connector/radio_connector.cpp`, `connector.cpp`,
`connector_config.cpp`). Decisions are listed at the end. See also
[ack-guide.md](ack-guide.md) (how the acks work, with the code) and
[rc1-test-messages.md](rc1-test-messages.md) (test messages to type in RadioConnectorCli) and
[json-format.md](json-format.md) (every JSON message and field, as a reference).
`RadioTests` checks all of this automatically, over the sound cards too.

The Radio Connector is a headless program that connects one radio (through direwolf,
AX.25 only) to the suite's main program. The main program sends it text and config as
JSON, and the connector delivers the text to another station's connector over the air.
RadioConnectorCli, the menu program, stays as it is, for editing settings by hand and
debugging packets.

```
 main program ──JSON/TCP── connector ──RF (AX.25)── connector ──JSON/TCP── client program
   (Repeater)                (A)                      (B)                   (User A)
```

There are two layers:

- **Suite JSON**: between the connector and the main program. It uses the shared
  envelope below, so other programs in the suite can use the same layout.
- **Air format**: compact text inside each AX.25 frame. It never leaves the connector;
  the main program only ever sees JSON.

---

## 1. Connection

- The connector **dials out** to the main program: `RadioConnector --main 127.0.0.1:9000`.
  The address is also kept in the cached config.
- If the connection drops, it retries every 5 seconds. The radio keeps running while the
  main program is away.
- Framing: one JSON object per line (UTF-8, `\n` at the end). No line may be longer than
  64 KiB.

## 2. Suite envelope

Every message, in both directions:

```json
{"v":1, "type":"message.send", "id":"m-102", "re":null, "body":{ ... }}
```

| Field | Meaning |
|---|---|
| `v` | Protocol version. Always 1 for now |
| `type` | Dotted name: `area.action` |
| `id` | Chosen by the sender, unique for that connection. Optional on events nobody replies to |
| `re` | On a reply: the `id` of the request it answers |
| `body` | Depends on `type` |

Errors, sent as a reply to any request:

```json
{"v":1,"type":"error","re":"m-102","body":{"code":"bad_field","field":"fx25","message":"must be off, auto, 16, 32 or 64"}}
```

## 3. Suite messages

### Connector → main

**`hello`**, sent right after connecting:
```json
{"v":1,"type":"hello","id":"c-hello","body":{
  "program":"radio-connector","version":"0.1.0","instance":"N0CALL-7",
  "capabilities":["ax25","fx25"],
  "config":{ ...cached config, see section 5... },
  "radio":"ready"}}
```

**`message.accepted`**, the reply to `message.send`:
```json
{"v":1,"type":"message.accepted","re":"m-102","body":{"msgId":"N0CALL-7/03F","parts":5}}
```

**`message.status`**, sent each time a message's state changes. This is the status
indicator:
```json
{"v":1,"type":"message.status","body":{
  "msgId":"N0CALL-7/03F","ref":"order-881","to":"K1ABC-3",
  "state":"retrying","parts":5,"partsAcked":[1,2,3,5],"attempt":2,"reason":null}}
```

| State | Meaning |
|---|---|
| `queued` | Accepted, waiting for the channel |
| `sending` | Parts going out |
| `waiting` | All parts sent, waiting for the other connector's ack |
| `retrying` | Resending the parts the other connector hasn't acked |
| `received` | The other connector has every part |
| `delivered` | The other connector's main program confirmed it |
| `expired` | The other connector held it for 3 min, but its main program never took it |
| `failed` | No ack after every retry. `reason` explains |

`delivered`, `expired` and `failed` are final. `received` means the other connector has
every part and its result is still on the way.

**`message.received`**, a whole reassembled message from another station:
```json
{"v":1,"type":"message.received","id":"c-55","body":{
  "msgId":"K1ABC-3/1A2","from":"K1ABC-3","to":"N0CALL-7","path":["K9XYZ-1*"],
  "text":"full reassembled text","parts":5,"firstHeard":"2026-10-28T15:02:11Z",
  "completed":"2026-10-28T15:02:19Z"}}
```
The main program must reply with `ack` (below). The connector resends the message after
every reconnect until it gets the ack, for up to 3 minutes.

**`radio.status`**, sent on every change and in reply to `status.get`:
```json
{"v":1,"type":"radio.status","body":{
  "state":"ready","direwolf":"running","ptt":"cat","txQueue":1,
  "inFlight":2,"heldForMain":0,"lastHeard":"2026-10-28T15:01:58Z","error":null}}
```
`state` is one of `unconfigured`, `starting`, `ready`, `restarting`, `error`. `error` says
what's wrong (e.g. `missing callsign`), or is null. After an error the connector starts
direwolf again every 10 s.

**`config`**, the reply to `config.get` and `config.set`:
```json
{"v":1,"type":"config","re":"m-7","body":{"config":{ ... },"restartRequired":true}}
```

**`audio.devices`**, the reply to `audio.list`. `name` is what `rxAudio` and `txAudio`
take (a sound card number on Windows); `desc` is for showing to people:
```json
{"v":1,"type":"audio.devices","re":"m-8","body":{
  "capture":[{"name":"0","desc":"Microphone (USB Audio CODEC)"}],
  "playback":[{"name":"0","desc":"Speakers (USB Audio CODEC)"}]}}
```

### Main → connector

| Type | Body | Reply |
|---|---|---|
| `message.send` | `{"to":"K1ABC-3","text":"...","ref":"order-881"}`. `ref` is optional, for the main program's own use, and comes back in every `message.status` | `message.accepted` or `error` |
| `ack` | `{}`, with `re` = the `id` of a `message.received` | none |
| `config.get` | `{}` | `config` |
| `config.set` | `{"config":{ only the fields to change }}` | `config` or `error` (nothing changes on an error) |
| `audio.list` | `{}` | `audio.devices` |
| `status.get` | `{}` | `radio.status` |
| `radio.restart` | `{}` | `radio.status` |

A `message.send` while the radio isn't `ready` is rejected with `radio_not_ready`.

Error codes: `bad_json`, `bad_version`, `bad_envelope`, `unknown_type`, `bad_field` (with
`field`), `too_long`, `radio_not_ready`. On reconnect the connector sends `hello`, a
`radio.status`, the current `message.status` of every unfinished message, and every
message still waiting for an `ack`.

## 4. Air format

Text inside the AX.25 info field. The AX.25 **source** is the sender and the AX.25
**destination** is the addressed station. A connector ignores frames that aren't
addressed to its callsign. Fields are separated by `|`, and the text is always the last
field, so it may contain `|`.

| Frame | Example | Meaning |
|---|---|---|
| Data | `RC1\|D\|03F\|2/5\|text...` | Message 03F, part 2 of 5 |
| Ack | `RC1\|A\|03F\|1-3,5` | Parts received so far |
| Result | `RC1\|R\|03F\|OK` | `OK` = delivered to the main program, `EXP` = held 3 min with no main program, `ERR` = bad message |

- `RC1` marks our frames, version 1, so other AX.25 traffic is ignored. A later version
  with hopping would be `RC2`.
- The message ID is 3 hex digits per sender and wraps at FFF. Together with the sender's
  callsign it's unique: the suite sees it as `N0CALL-7/03F`.
- Text per part = the packet limit minus the header, about 163 bytes with FX.25 auto.
  Parts are filled to the limit (words may be split across parts), never cutting a
  UTF-8 character. Reassembly restores the exact text.
- At most **40 parts** per message (about 6.5 KB with FX.25 auto). Bigger messages are
  rejected with `too_long`.

### Sending

1. Split the text, then send every part.
2. Wait for an ack. The wait is the estimated airtime of every part, plus 10 s.
3. If the ack is missing parts, resend only those. With no ack at all, resend the last
   part, which makes the other connector ack again.
4. Up to 4 tries (configurable), then `failed`. An ack that's missing parts after
   everything has gone out starts the next try straight away.
5. When every part is acked: `received`. Then wait up to 3 min + 30 s for the result
   frame. `OK` → `delivered`, `EXP` → `expired`, `ERR` → `failed`.
6. If no result arrives, resend the last part (the other side acks and sends its result
   again), using the tries left. Then `failed`.
7. One message is on the air at a time: the next leaves `queued` once this one is
   `received` or finished. Acks and results go out whenever they're due.
8. While the radio is down, these waits pause, so a restart doesn't use up tries.

### Receiving

1. Collect parts by sender and message ID.
2. Ack 2 s after the last part heard, or straight away once all parts are in.
3. Hand the whole message to the main program as `message.received`.
4. When the main program acks: send `RC1|R|<id>|OK`. If there's no ack within 3 minutes
   (no main program connected, or no reply): send `EXP` and drop the message.
5. Remember finished message IDs for 10 min. A repeat part from a finished message gets
   its ack, and its result again, but the main program doesn't see it twice.
6. Drop a message that hasn't got every part when 2 min pass with no new part heard.

### Ready for hopping

The code keeps two addresses apart: the **link** (the AX.25 frame, from this station to
the next one) and the **message** (from the original sender to the final destination,
message ID, parts). Today the two are always the same. Relaying through other
connectors would come in `RC2`, which carries the original sender and final destination
in the header, without changing the suite JSON. Ordinary digipeaters already work
through the AX.25 path (`path` in config).

## 5. Config

The main program owns the config. The connector caches the last one it received, so it
can start the radio before the main program connects. `config.set` takes any subset:

```json
{
  "callsign":"N0CALL-7",
  "packetBaud":1200,
  "fx25":"auto",
  "path":[],
  "rxAudio":"...", "txAudio":"...",
  "bluetooth":{"address":"", "playAudio":"", "recordAudio":""},
  "radio":{"type":"cat","port":"COM5","baud":38400,"rigModel":1037},
  "ptt":{"type":"rts","port":"COM6"},
  "retry":{"tries":4,"extraWaitSeconds":10},
  "holdForMainSeconds":180,
  "main":"127.0.0.1:9000"
}
```

Changing `callsign`, `packetBaud`, `fx25`, the audio devices, `bluetooth`, `radio` or `ptt` restarts
direwolf. The reply says so with `restartRequired: true`, and messages in flight are kept.

direwolf keys the radio itself; the connector never sets the frequency or mode.

| Field | Values |
|---|---|
| `radio.type` | `none`; `cat` (a radio on `port`, with `baud` and Hamlib `rigModel`); `rigctld` (your own rigctld, `port` = host:port) |
| `ptt.type` | `vox` (the default, e.g. FM on VHF/UHF); `cat` (a command over the radio's CAT port, needs `radio`); `rts` or `dtr` (that line of a serial port) |
| `bluetooth.address` | A UV-Pro-style radio's Bluetooth address, e.g. `38:D2:00:01:C0:AB`, for audio over Bluetooth ([bluetooth-audio.md](bluetooth-audio.md)); `""` (the default) = sound cards. Setting one sets `ptt` to `vox`: the radio keys itself. On Linux and macOS `rxAudio`/`txAudio` are then unused |
| `bluetooth.playAudio`, `bluetooth.recordAudio` | Windows only: the virtual cable ends the bridge plays the radio's audio into and records the transmit audio from (WinMM numbers, as in `audio.devices`). `rxAudio`/`txAudio` are the cables' other ends |
| `ptt.port` | `rts`/`dtr` only: the serial port, e.g. a second USB port like the FTDX3000's Standard port. `""` = the radio's CAT port |

`radio` and `ptt` are checked together: `ptt` `cat` without a `radio`, or `rts` on a
rigctld address, is an error (field `ptt`). Change both in one `config.set`.

The cache is the connector's own `connector.ini`, next to RadioConnectorCli's
`settings.ini` (or the file given with `--settings`). The first run copies `settings.ini`,
so menu settings (sound cards, PTT, baud) carry over, and takes the next SSID as its
callsign, so the two programs are different stations. The connector always uses direwolf,
whatever the file's `modem` line says. Connector-only keys (path, retry, hold time, main
address) are in the same format; RadioConnectorCli ignores them.

---

## Decisions

- The receiver acks 2 s after the last part it hears (straight away once it has every
  part), then sends a second result frame (delivered, or expired after 3 minutes).
- The config cache is the connector's own `connector.ini`, first copied from `settings.ini`.
- Parts are split at the byte limit.
- At most 40 parts per message.
- FX.25 is on (Auto) by default and goes to every station: a plain AX.25 station still
  decodes FX.25 packets, without the error correction. No per-station fallback.
- A partial message is dropped after 2 min with no new part, not 2 min after the first.
- One message on the air at a time.
- A lost result frame: poke with the last part, then `failed`.
- JSON: nlohmann/json 3.11.3, copied into `src/third_party/nlohmann`.
