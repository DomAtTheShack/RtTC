# Radio Connector protocol

The Radio Connector is a headless program that connects one radio (through direwolf,
AX.25 with FX.25) to a **main program**: a separate application, not released yet, that
answers the people on the radio, e.g. an LLM chat. Any program that speaks the JSON below
can be the main program. RadioConnectorCli, the menu program, is for setting things up by
hand and testing.

People reach the connector with **AX.25 connected mode**: any packet station connects to
the connector's callsign and gets a session, like a packet BBS
([connected-mode.md](connected-mode.md)). The main program can open sessions too.

```
 packet station ──AX.25 connected── direwolf ──AGW── RadioConnector ──JSON/TCP── main program
 (any station)                      (link layer)     (sessions)                  (LLM chat)
```

This page is the overview. [json-format.md](json-format.md) lists every message and field;
[connected-mode.md](connected-mode.md) explains sessions, paging and limits. The code is
`src/Connector/radio_connector.cpp` (the program), `sessions.cpp` (sessions),
`connector_config.cpp` (config) and `connector.cpp` (the envelope). `RadioTests` checks it
all, over the sound cards too.

## Connection

- The connector **dials out** to the main program: `RadioConnector --main 127.0.0.1:9000`.
  The address is saved, so later runs don't need `--main`.
- If the connection drops, it tries again every 5 seconds. The radio keeps running while
  the main program is away; stations that connect meanwhile are told it's offline.
- One JSON object per line (UTF-8, `\n` at the end), at most 64 KiB.

## Envelope

Every message, in both directions:

```json
{"v":1, "type":"message.send", "id":"m-102", "re":null, "body":{ ... }}
```

| Field | Meaning |
|---|---|
| `v` | Protocol version. Always 1 for now |
| `type` | Dotted name: `area.action` |
| `id` | Chosen by the sender. Put one on any request you want a reply to |
| `re` | On a reply: the `id` of the request it answers |
| `body` | Depends on `type` |

Errors are a reply to the request that failed:

```json
{"v":1,"type":"error","re":"m-102","body":{"code":"bad_field","field":"fx25","message":"must be off, auto, 16, 32 or 64"}}
```

## Messages

| Main → connector | Connector → main |
|---|---|
| `session.open`, `session.close`, `session.list` | `hello` (on connect), `radio.status` (on any change) |
| `message.send` (text to a session) | `session.opening`, `session.opened`, `session.new`, `session.closing`, `session.closed`, `sessions` |
| `config.get`, `config.set` | `message.received` (a line someone typed), `message.accepted`, `message.status` |
| `audio.list`, `status.get`, `radio.restart` | `config`, `audio.devices`, `error` |

A typical exchange: a station connects (`session.opened`), types a line
(`message.received`), and the main program answers it (`message.send` with the
`session`). The connector pages long answers and reports each one's progress
(`message.status`). Details and examples are in [json-format.md](json-format.md).

## Config

The main program owns the config. The connector keeps the last one in its own
`connector.ini`, next to RadioConnectorCli's `settings.ini` (or the file given with
`--settings`), so it can start the radio before the main program connects. `config.set`
takes any subset of the fields ([json-format.md](json-format.md#config)):

```json
{
  "callsign":"N0CALL-7",
  "packetBaud":1200,
  "fx25":"auto",
  "path":[],
  "rxAudio":"...", "txAudio":"...",
  "bluetooth":{"address":"", "playAudio":"", "recordAudio":""},
  "radio":{"type":"cat","port":"COM5","baud":19200,"rigModel":3073},
  "ptt":{"type":"rts","port":"COM6"},
  "main":"127.0.0.1:9000",
  "connected":{"enabled":true,"maxSessions":4,"idleMinutes":10,"pageBytes":1024,
               "busyText":"Busy, try again in a few minutes"}
}
```

Changing `callsign`, `packetBaud`, `fx25`, the audio devices, `bluetooth`, `radio` or `ptt`
restarts direwolf; the reply says so with `restartRequired: true`, and open sessions close.
direwolf keys the radio itself; the connector never sets the frequency or mode.

The first run copies `settings.ini`, so the menu program's settings (sound cards, PTT, baud)
carry over, and takes the next SSID as its callsign (N0CALL becomes N0CALL-1), so the two
programs are different stations. Connector-only keys (`path`, `main`, `connected`) are in
the same file format; RadioConnectorCli ignores them.

## Design choices

- The main program never sees the radio: direwolf, AX.25 and paging stay in the connector.
- direwolf runs the AX.25 link (connects, acks, retries); the connector drives it over
  direwolf's AGW port.
- FX.25 is on (Auto) by default. A station without FX.25 still decodes the frames, without
  the error correction.
- The suite is for VHF/UHF only.
- JSON: nlohmann/json 3.11.3, copied into `src/third_party/nlohmann`.

`RC1`, an earlier protocol of our own for messages between two connectors in UI frames,
was removed: connected mode lets any packet station use the connector, and `RC1`'s
advantages mostly mattered on HF.
