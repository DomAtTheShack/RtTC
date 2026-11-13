# JSON format

A reference for every JSON message between `RadioConnector` and the main program: the
envelope, each message type and its fields, the config object, and the errors. The main
program is a separate application, not released yet; any program that speaks this format
can take its place. People reach it through [connected sessions](#connected-sessions)
(AX.25 connected mode): any packet station can connect, and the main program can open
sessions too. [connected-mode.md](connected-mode.md) explains how they work, and
[connector-protocol.md](connector-protocol.md) gives the overview.

This page is only the format. The code is in `src/Connector/radio_connector.cpp`
(requests), `sessions.cpp` (sessions) and `connector_config.cpp` (config).

## Connection and framing

- The connector connects **out** to the main program over TCP
  (`RadioConnector --main 127.0.0.1:9000`, or `main` in the config). The main program
  listens. IPv4 addresses only; `localhost` means `127.0.0.1`.
- Each message is **one JSON object on one line**, UTF-8, ending with `\n`.
- A line may be at most **64 KiB**. A longer line is dropped and answered with a
  `too_long` error.
- If the connection drops, the connector tries again every 5 seconds.

## Envelope

Every message, in both directions, has the same outer layout:

```json
{"v":1,"type":"message.send","id":"m-102","re":null,"body":{}}
```

| Field | Type | Meaning |
|---|---|---|
| `v` | integer | Protocol version. Must be `1` |
| `type` | string | What the message is, e.g. `message.send` |
| `id` | any (usually a string) | Chosen by the sender. Put one on every request you want a reply to: it comes back as `re` |
| `re` | same as the `id` it answers, or `null` | On a reply: the `id` of the request it answers |
| `body` | object | Depends on `type`. May be left out on requests with no fields; it's then `{}` |

What the connector sends:

- Always `v`, `type`, `re` and `body`. `re` is `null` when the message isn't a reply.
- `id` only on `hello` (`"c-hello"`).

What the connector checks on each line, in this order (the first failure is the reply):

1. It's a JSON object, or `bad_json`.
2. `v` is `1`, or `bad_version`.
3. `type` is a string, or `bad_envelope`.
4. `body`, if there, is an object, or `bad_envelope` (field `body`).
5. `type` is one it knows, or `unknown_type` (field `type`).

## Common values

| Value | Format |
|---|---|
| Callsign | 1 to 6 letters or digits, then optionally `-` and an SSID from 0 to 15, e.g. `K1ABC-3`. The connector makes it upper case, strips spaces and drops `-0` |
| Session ID | `s-` and a number, e.g. `s-3`. Not reused while the connector runs |
| Message ID (`msgId`) | The session, then `/t` and a number for text sent (`s-3/t1`), or `/r` for a line received (`s-3/r2`) |
| Time | UTC, ISO 8601, to the second: `2026-10-28T15:02:11Z` |

## Main program → connector

| `type` | `body` | Reply |
|---|---|---|
| [`message.send`](#messagesend) | `{"session", "text", "ref"?}` | `message.accepted`, or `error` |
| [`session.open`](#sessionopen) | `{"to", "path"?}` | `session.opening`, or `error` |
| [`session.close`](#sessionclose) | `{"session", "text"?}` | `session.closing`, or `error` |
| [`session.list`](#sessionlist) | `{}` | `sessions` |
| [`config.get`](#configget) | `{}` | `config` |
| [`config.set`](#configset) | `{"config": {...}}` | `config`, or `error` |
| [`audio.list`](#audiolist) | `{}` | `audio.devices` |
| [`status.get`](#statusget) | `{}` | `radio.status` |
| [`radio.restart`](#radiorestart) | `{}` | `radio.status` |

### `config.get`

Asks for the current config. The reply is `config` with `restartRequired: false`.

```json
{"v":1,"type":"config.get","id":"m-6","body":{}}
```

### `config.set`

Changes the config. `config` holds only the fields to change; see [Config](#config).

```json
{"v":1,"type":"config.set","id":"m-7","body":{"config":{"callsign":"N0CALL-7","fx25":"auto"}}}
```

The whole change is checked first. On any error nothing changes, and the reply is a
`bad_field` error naming the field (e.g. `ptt.port`). Otherwise the change is saved to
`connector.ini` and the reply is the full new `config`. If the change needs direwolf to
restart (`restartRequired: true`), it restarts after the reply, and open sessions close
(reason `radio_down`). Changing `main` makes the connector disconnect and dial the new
address.

### `audio.list`

Lists the sound cards. The reply is `audio.devices`.

### `status.get`

Asks for the radio's state. The reply is `radio.status`.

### `radio.restart`

Restarts direwolf. The reply is `radio.status`, usually with `state: "restarting"`.

## Connector → main program

| `type` | When | Answer it with |
|---|---|---|
| [`hello`](#hello) | Right after connecting | nothing |
| [`message.accepted`](#messagesend) | Reply to `message.send` | nothing |
| [`message.status`](#messagesend) | Each time text sent to a session changes state | nothing |
| [`message.received`](#messagereceived) | A line was typed in a session | nothing |
| [`session.opening`](#sessionopen) | Reply to `session.open` | nothing |
| [`session.opened`](#sessionopened) | A session is open (either way), and again on every reconnect | nothing |
| [`session.new`](#sessionnew) | The user typed `N` | start a new conversation |
| [`session.closing`](#sessionclose) | Reply to `session.close` | nothing |
| [`session.closed`](#sessionclosed) | A session has ended | nothing |
| [`sessions`](#sessionlist) | Reply to `session.list` | nothing |
| [`radio.status`](#radiostatus) | Any change, and reply to `status.get` / `radio.restart` | nothing |
| [`config`](#config-reply) | Reply to `config.get` / `config.set` | nothing |
| [`audio.devices`](#audiodevices) | Reply to `audio.list` | nothing |
| [`error`](#errors) | A request went wrong | nothing |

On every connect (and reconnect) the connector sends, in this order:

1. `hello`
2. a `session.opened` for each open session
3. `radio.status`

### `hello`

```json
{"v":1,"type":"hello","id":"c-hello","re":null,"body":{
  "program":"radio-connector","version":"0.1.2","instance":"N0CALL-7",
  "capabilities":["ax25","fx25","connected"],"config":{},"radio":"ready"}}
```

| Field | Type | Meaning |
|---|---|---|
| `program` | string | Always `radio-connector` |
| `version` | string | The connector's version |
| `instance` | string | This station's callsign. `""` if not set yet |
| `capabilities` | string array | `["ax25","fx25","connected"]` |
| `config` | object | The full cached [config](#config) |
| `radio` | string | The radio's `state`, as in `radio.status` |

### `radio.status`

Sent whenever any field changes, and as the reply to `status.get` and `radio.restart`
(then with `re` set).

```json
{"v":1,"type":"radio.status","re":null,"body":{
  "state":"ready","direwolf":"running","ptt":"cat","sessions":2,
  "lastHeard":"2026-10-28T15:01:58Z","error":null}}
```

| Field | Type | Meaning |
|---|---|---|
| `state` | string | `unconfigured` (something required is missing, see `error`), `starting`, `ready`, `restarting`, `error` |
| `direwolf` | string | `stopped`, `starting`, `running` |
| `ptt` | string | How the radio is keyed: `vox`, `cat`, `rts`, `dtr`, `aioc` |
| `sessions` | integer | Connected sessions open or opening |
| `lastHeard` | time or `null` | When another station last connected or sent session text |
| `error` | string or `null` | What's wrong, e.g. `missing callsign`. After an error the connector tries direwolf again every 10 s |

`message.send` and `session.open` only work while `state` is `ready`.

### `config` (reply)

```json
{"v":1,"type":"config","re":"m-7","body":{"config":{},"restartRequired":true}}
```

| Field | Type | Meaning |
|---|---|---|
| `config` | object | The full [config](#config), after the change |
| `restartRequired` | boolean | `true` if direwolf restarts to apply it |

### `audio.devices`

```json
{"v":1,"type":"audio.devices","re":"m-8","body":{
  "capture":[{"name":"0","desc":"Microphone (USB Audio CODEC)"}],
  "playback":[{"name":"0","desc":"Speakers (USB Audio CODEC)"}]}}
```

| Field | Type | Meaning |
|---|---|---|
| `capture` | array | Recording devices, for `rxAudio` and `bluetooth.recordAudio` |
| `playback` | array | Playback devices, for `txAudio` and `bluetooth.playAudio` |
| `[].name` | string | What the config takes. A sound card number on Windows |
| `[].desc` | string | A name for people |

## Connected sessions

An AX.25 connection with one other station: any packet terminal that connects to this
station, or one the main program opens with `session.open`. The user types a line, the
main program gets it as `message.received`, and answers with `message.send`.

- **One line typed is one `message.received`.** Line ends (CR, LF) aren't included.
- **Text sent** may use `\n`; the connector sends CR line ends, as packet terminals expect.
- **Long replies are paged** (`connected.pageBytes`). After each page but the last the user
  sees `[M]ore, [N]ew, [B]ye`. The connector handles these itself:

  | The user types | What happens |
  |---|---|
  | `M` or `MORE` | The next page |
  | `N` or `NEW` | The rest is dropped (`skipped`), and the main program gets [`session.new`](#sessionnew) |
  | `B` or `BYE` | The session closes, reason `bye` |
  | anything else | The rest is dropped (`skipped`), and the line is a new `message.received` |

- **Nothing to acknowledge.** The main program doesn't answer `message.received`, and
  text sent can't be confirmed read: AX.25 only says the other station's radio got it.
- **If the main program isn't connected**, a station that connects is told "The
  repeater's program is offline" and disconnected, and a line typed in an open session
  gets the same notice.

### `session.open`

Opens a session to another station.

```json
{"v":1,"type":"session.open","id":"m-20","body":{"to":"K1ABC-3"}}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `to` | string | yes | The other station's callsign |
| `path` | string array | no | Up to 7 digipeaters. Default: the config's `path`. `[]` = direct |

The reply comes straight away:

```json
{"v":1,"type":"session.opening","re":"m-20","body":{"session":"s-4","peer":"K1ABC-3"}}
```

Then `session.opened` once the other station answers, or `session.closed` with reason
`no_answer`. `message.send` to a session that's still opening waits until it's open.

Errors: `connected_off`, `radio_not_ready`, `too_many_sessions`, `bad_field` (`to`: not
a callsign, this station's own, or already in a session; `path`).

### `session.opened`

```json
{"v":1,"type":"session.opened","re":null,"body":{"session":"s-4","peer":"K1ABC-3","direction":"out"}}
```

| Field | Type | Meaning |
|---|---|---|
| `session` | string | The session's ID |
| `peer` | string | The other station's callsign |
| `direction` | string | `in` (they connected) or `out` (`session.open`) |

### `message.send`

```json
{"v":1,"type":"message.send","id":"m-21","body":{"session":"s-4","text":"Here's the answer...","ref":"chat-9"}}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `session` | string | yes | The session's ID |
| `text` | string | yes | Not empty, valid UTF-8. No length limit beyond the 64 KiB line |
| `ref` | any | no | Yours to use. Comes back unchanged in every `message.status`. `null` if left out |

The reply is `message.accepted`, with how many pages the text was split into:

```json
{"v":1,"type":"message.accepted","re":"m-21","body":{"msgId":"s-4/t1","session":"s-4","pages":3}}
```

Then a `message.status` on each change:

```json
{"v":1,"type":"message.status","re":null,"body":{
  "msgId":"s-4/t1","session":"s-4","ref":"chat-9","to":"K1ABC-3",
  "state":"paused","pages":3,"pagesSent":1,"reason":null}}
```

| `state` | Meaning | Final |
|---|---|---|
| `queued` | Accepted, not started yet (e.g. behind another reply in the same session) | |
| `sending` | A page is going out | |
| `paused` | A page is out and acked; waiting for the user to type `M` | |
| `received` | Every page is out and acked by the other station's radio | yes |
| `skipped` | The user moved on (`N`, or a new line) before reading every page | yes |
| `failed` | The session closed first. `reason` says why | yes |

Errors: `radio_not_ready` (the radio isn't `ready`), `bad_field` (`session`: missing, no
such session, or it's closing; `text`).

### `message.received`

```json
{"v":1,"type":"message.received","re":null,"body":{
  "msgId":"s-4/r2","session":"s-4","from":"K1ABC-3","to":"N0CALL-7",
  "text":"What's the weather tomorrow?",
  "firstHeard":"2026-11-06T15:02:11Z","completed":"2026-11-06T15:02:11Z"}}
```

One line the user typed, without its line end. There's no `id`: nothing answers it.

### `session.new`

```json
{"v":1,"type":"session.new","re":null,"body":{"session":"s-4","peer":"K1ABC-3"}}
```

The user typed `N`: forget the conversation so far and start again.

### `session.close`

Closes a session, optionally with a last line.

```json
{"v":1,"type":"session.close","id":"m-22","body":{"session":"s-4","text":"73!"}}
```

The reply is `session.closing` with `session` and `peer`. Replies already queued for the
session go out first (a `paused` one is `skipped`), then `text`, then the connector
disconnects and sends `session.closed` with reason `closed`.

### `session.closed`

```json
{"v":1,"type":"session.closed","re":null,"body":{"session":"s-4","peer":"K1ABC-3","reason":"bye"}}
```

| `reason` | Meaning |
|---|---|
| `bye` | The user typed `B` |
| `closed` | The main program sent `session.close` |
| `remote` | The other station disconnected |
| `idle` | Nothing sent either way for `connected.idleMinutes` |
| `link_lost` | The other station stopped answering |
| `no_answer` | `session.open`: the other station never answered |
| `replaced` | The same station connected again; a new session takes over |
| `radio_down` | direwolf stopped or restarted |
| `disabled` | `connected.enabled` was turned off |
| `restart` | The callsign changed |

### `session.list`

```json
{"v":1,"type":"sessions","re":"m-23","body":{"sessions":[
  {"session":"s-4","peer":"K1ABC-3","direction":"out","state":"open","queued":1}]}}
```

`state` is `connecting`, `open` or `closing`; `queued` counts replies not yet finished.

## Config

`config.get`, `config.set`, and `hello` all use this object. The connector always sends
every field; `config.set` takes any of them. Inside `bluetooth`, `radio`, `ptt` and
`connected`, only the fields given change. Unknown fields are a `bad_field` error.

```json
{
  "callsign": "N0CALL-7",
  "packetBaud": 1200,
  "fx25": "auto",
  "txDelay": 500,
  "path": [],
  "rxAudio": "0",
  "txAudio": "0",
  "bluetooth": {"address": "", "playAudio": "", "recordAudio": ""},
  "radio": {"type": "cat", "port": "COM5", "baud": 19200, "rigModel": 3073},
  "ptt": {"type": "rts", "port": "COM6"},
  "main": "127.0.0.1:9000",
  "connected": {"enabled": true, "maxSessions": 4, "idleMinutes": 10, "pageBytes": 1024,
                "busyText": "Busy, try again in a few minutes"}
}
```

| Field | Type | Default | Values | Restarts direwolf |
|---|---|---|---|---|
| `callsign` | string | `""` | A [callsign](#common-values). Required before the radio starts | yes |
| `packetBaud` | integer | `1200` | `300`, `1200`, `9600` | yes |
| `fx25` | string or integer | `"auto"` | `"off"`, `"auto"`, `16`, `32`, `64` (check bytes; `"16"` etc. also accepted). Sent back as `"off"`, `"auto"` or a number | yes |
| `txDelay` | integer | `500` | 0 to 2550: milliseconds from keying up to the packet (direwolf's TXDELAY, rounded to 10 ms). Raise it if the start of packets gets cut off | yes |
| `path` | string array | `[]` | Up to 7 digipeater callsigns, e.g. `["WIDE1-1"]`, for `session.open` | no |
| `rxAudio` | string | `""` | Recording device `name` from `audio.devices`. Required, except with Bluetooth on Linux/macOS or `ptt.type` `aioc` | yes |
| `txAudio` | string | `""` | Playback device `name`. Required, except with Bluetooth on Linux/macOS or `ptt.type` `aioc` | yes |
| `bluetooth.address` | string | `""` | A radio's Bluetooth address, e.g. `38:D2:00:01:C0:AB` (`-` also accepted), for audio over Bluetooth. `""` = use sound cards. Setting one sets `ptt.type` to `vox` | yes |
| `bluetooth.playAudio` | string | `""` | Windows only: the virtual cable the radio's audio plays into. Required on Windows with Bluetooth | yes |
| `bluetooth.recordAudio` | string | `""` | Windows only: the virtual cable the transmit audio is recorded from. Required on Windows with Bluetooth | yes |
| `radio.type` | string | `"none"` | `none`, `cat`, `rigctld` | yes |
| `radio.port` | string | | `cat`: a serial port, e.g. `COM5` or `/dev/ttyUSB0`. `rigctld`: `host:port` of your own rigctld. Required when switching to either | yes |
| `radio.baud` | integer | `19200` | `cat` only: 300 to 1000000 | yes |
| `radio.rigModel` | integer | `3087` | `cat` only: a Hamlib model number, 1 to 99999 (not 2; use `rigctld`) | yes |
| `ptt.type` | string | `"vox"` | `vox` (the radio keys on audio), `cat` (a command over the radio's port, needs `radio`), `rts` or `dtr` (that line of a serial port), `aioc` (an [AIOC cable](radios.md#aioc-cable): its serial port and sound card are found each time the radio starts, and replace `rxAudio`/`txAudio`) | yes |
| `ptt.port` | string | `""` | `rts`/`dtr` only: the serial port. `""` = the radio's CAT port. Sent for `rts`/`dtr`, and for `aioc` as the port the cable was last found on (read-only: setting it with `aioc` is an error) | yes |
| `main` | string | | `host:port` of the main program, IPv4 or `localhost` | no (reconnects) |
| `connected.enabled` | boolean | `true` | Accept incoming sessions and allow `session.open`. `false` closes every session | no |
| `connected.maxSessions` | integer | `4` | 1 to 16. A station that connects when it's full gets `busyText` and is disconnected | no |
| `connected.idleMinutes` | integer | `10` | 0 to 1440. A session with nothing sent either way this long is closed. 0 = never | no |
| `connected.pageBytes` | integer | `1024` | 0 (no paging) or 200 to 65536. Replies longer than this go out a page at a time | no |
| `connected.busyText` | string | `"Busy, try again in a few minutes"` | 1 to 200 bytes, one line | no |

`radio` comes back with only the fields for its type: `{"type":"none"}`,
`{"type":"rigctld","port":"127.0.0.1:4532"}` or
`{"type":"cat","port":"COM5","baud":19200,"rigModel":3073}`.

`radio`, `ptt` and `bluetooth` are checked together once the whole change is in. These
are errors with field `ptt`:

- `ptt` `cat` without a `radio`
- `rts`/`dtr` with no serial port (no `ptt.port` and no `cat` radio), or on the radio's
  port when that's a rigctld address
- anything but `vox` while `bluetooth.address` is set

With `aioc`, a missing cable isn't a config error: `radio.status` goes to `error` (e.g.
`No AIOC cable found. Is it plugged in?`), and the connector tries again every 10 s.

To change more than one of them, send them in one `config.set`.

The connector never sets the radio's frequency or mode.

## Errors

Sent as a reply to the request that failed (`re` = its `id`, or `null` if it had none or
the line couldn't be read).

```json
{"v":1,"type":"error","re":"m-102","body":{"code":"bad_field","field":"fx25","message":"must be off, auto, 16, 32 or 64"}}
```

| Field | Type | Meaning |
|---|---|---|
| `code` | string | See below. Use this in code |
| `message` | string | For people. The wording may change |
| `field` | string | The field at fault, e.g. `to` or `radio.port`. Only on some errors |

| `code` | When |
|---|---|
| `bad_json` | The line isn't a JSON object |
| `bad_version` | `v` is missing or isn't `1` |
| `bad_envelope` | `type` isn't a string, or `body` isn't an object |
| `unknown_type` | No such request (field `type`) |
| `bad_field` | A body or config field is missing or wrong (field = which one) |
| `too_long` | A line over 64 KiB |
| `radio_not_ready` | `message.send` or `session.open` while the radio isn't `ready` |
| `connected_off` | `session.open` while `connected.enabled` is `false` |
| `too_many_sessions` | `session.open` with `connected.maxSessions` already open |

## Example session

`>` is the connector, `<` the main program.

```
> {"v":1,"type":"hello","id":"c-hello","re":null,"body":{"program":"radio-connector","version":"0.1.2","instance":"N0CALL-7","capabilities":["ax25","fx25","connected"],"config":{...},"radio":"ready"}}
> {"v":1,"type":"radio.status","re":null,"body":{"state":"ready","direwolf":"running","ptt":"vox","sessions":0,"lastHeard":null,"error":null}}
```

Then K1ABC-3 connects from a packet terminal and asks something (`radio.status` lines in
between are left out):

```
> {"v":1,"type":"session.opened","re":null,"body":{"session":"s-1","peer":"K1ABC-3","direction":"in"}}
> {"v":1,"type":"message.received","re":null,"body":{"msgId":"s-1/r1","session":"s-1","from":"K1ABC-3","to":"N0CALL-7","text":"Tell me about FX.25","firstHeard":"2026-11-06T15:02:11Z","completed":"2026-11-06T15:02:11Z"}}
< {"v":1,"type":"message.send","id":"m-2","body":{"session":"s-1","text":"FX.25 adds forward error correction..."}}
> {"v":1,"type":"message.accepted","re":"m-2","body":{"msgId":"s-1/t1","session":"s-1","pages":2}}
> {"v":1,"type":"message.status","re":null,"body":{"msgId":"s-1/t1","session":"s-1","ref":null,"to":"K1ABC-3","state":"queued","pages":2,"pagesSent":0,"reason":null}}
> {"v":1,"type":"message.status","re":null,"body":{"msgId":"s-1/t1","session":"s-1","ref":null,"to":"K1ABC-3","state":"sending","pages":2,"pagesSent":0,"reason":null}}
> {"v":1,"type":"message.status","re":null,"body":{"msgId":"s-1/t1","session":"s-1","ref":null,"to":"K1ABC-3","state":"paused","pages":2,"pagesSent":1,"reason":null}}
  (the user types M)
> {"v":1,"type":"message.status","re":null,"body":{"msgId":"s-1/t1","session":"s-1","ref":null,"to":"K1ABC-3","state":"sending","pages":2,"pagesSent":1,"reason":null}}
> {"v":1,"type":"message.status","re":null,"body":{"msgId":"s-1/t1","session":"s-1","ref":null,"to":"K1ABC-3","state":"received","pages":2,"pagesSent":2,"reason":null}}
  (the user types B)
> {"v":1,"type":"session.closed","re":null,"body":{"session":"s-1","peer":"K1ABC-3","reason":"bye"}}
```
