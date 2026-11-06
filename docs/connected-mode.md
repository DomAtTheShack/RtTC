# Connected mode

Status: **built** on the `connected-mode` branch, and **on by default**
(`connected.enabled`). Connected mode is the main way to reach the connector; `RC1` stays
for now and will be removed ([below](#removing-rc1)). The suite is for VHF/UHF only, never
HF.

Connected mode lets an ordinary packet station (a TNC, a terminal program, an app) talk to
the connector without our software: it types `connect N0CALL-7` and gets a session, like a
packet BBS. The main program, e.g. an LLM chat, answers through the same JSON as `RC1`
messages. It works both ways: the main program can also open a session to another station.

`RC1` (UI frames, [connector-protocol.md](connector-protocol.md)) still works beside it
on the same radio, until it's removed.

```
 packet terminal ──AX.25 connected── direwolf ──AGW── connector ──JSON── main program
 (any station)       (SABM, I, RR)    (link layer)      (sessions)        (LLM chat)
```

## How it works

direwolf runs the AX.25 link itself (connect, sequence numbers, acks, retries) and the
connector drives it over direwolf's **AGW** port, next to the KISS port `RC1` uses. The
connector only sees connects, disconnects and data. RadioConnectorCli doesn't change: it
keeps using KISS, and its packet screen shows the connected-mode frames (`SABM`, `UA`, `I`,
`RR`) like any others.

| AGW | Used for |
|---|---|
| `X` | Register the connector's callsign, so direwolf accepts incoming connects for it. Only while `connected.enabled` |
| `C`, `v` | Open a session (`v` through the config's digipeater `path`). From direwolf: a session is open |
| `D` | Session data, both ways |
| `d` | Close a session. From direwolf: a session has closed |
| `Y` | How many frames of a session direwolf still has to send or get acked |

## Several users at once

Everyone shares one half-duplex channel, so one user's long answer mustn't hold up the rest.

1. **Small amounts at a time, taking turns.** The connector keeps each session's outgoing
   text itself and gives direwolf at most one window (4 frames) per session. It asks with
   `Y` how many are still out and tops the session up when they're acked. direwolf then
   sends the sessions' frames in turn, so user B never waits for all of user A's answer.
2. **Paging.** A long reply goes out a page at a time (`pageBytes`, default 1024). After
   each page but the last the user sees `[M]ore, [N]ew, [B]ye`:
   - `M` (or `MORE`): the next page.
   - `N` (or `NEW`): drop the rest, and tell the main program to start a new conversation.
   - `B` (or `BYE`): disconnect.
   - Anything else: drop the rest of the reply and treat the line as the next prompt.
3. **Limits.** At most `maxSessions` sessions (default 4). A station that connects when
   it's full gets `busyText` and is disconnected; the main program never sees it. A session
   with nothing sent either way for `idleMinutes` (default 10) is told so and closed. A
   station that connects again replaces its old session.
4. **The main program never blocks the radio.** Prompts go to it as they arrive; replies go
   out whenever it sends them. If it isn't connected, a prompt gets "The repeater's program
   is offline" back.

Each frame carries at most what fits in one FX.25 block (about 180 bytes with FX.25 auto),
and direwolf's `PACLEN` is set to match, so FX.25 still protects every frame.

direwolf throws away unsent data when told to disconnect, so a goodbye (`busyText`, the
idle notice, `session.close` with `text`) is sent first, and the disconnect follows once
`Y` says it's all acked (or after 60 s).

## Config

```json
"connected": {
  "enabled": true,
  "maxSessions": 4,
  "idleMinutes": 10,
  "pageBytes": 1024,
  "busyText": "Busy, try again in a few minutes"
}
```

| Field | Values |
|---|---|
| `enabled` | `true` registers the callsign with direwolf, so incoming connects are accepted, and allows `session.open`. Turning it off closes every session |
| `maxSessions` | 1 to 16 |
| `idleMinutes` | 0 to 1440. 0 = never |
| `pageBytes` | 0 = no paging, else 200 to 65536 |
| `busyText` | Sent to a station that connects when it's full. Up to 200 bytes |

None of them restart direwolf.

## JSON

The full field list is in [json-format.md](json-format.md#connected-sessions). In short:

| Type | Direction | Body |
|---|---|---|
| `session.open` | main → connector | `{"to":"K1ABC-3"}`. Reply `session.opening` or `error` |
| `session.opened` | connector → main | `{"session":"s-3","peer":"K1ABC-3","direction":"in"}` |
| `session.closed` | connector → main | `{"session":"s-3","peer":"K1ABC-3","reason":"bye"}` |
| `session.new` | connector → main | `{"session":"s-3","peer":"K1ABC-3"}`: the user typed `N` |
| `session.opening` | connector → main | The reply to `session.open`: `{"session":"s-4","peer":"K1ABC-3"}` |
| `session.close` | main → connector | `{"session":"s-3","text":"73!"}`: send `text` (optional), then disconnect. Reply `session.closing` |
| `session.list` | main → connector | `{}`. Reply `sessions` |
| `message.received` | connector → main | One line the user typed, with `session` |
| `message.send` | main → connector | `{"session":"s-3","text":"..."}` instead of `to` |
| `message.status` | connector → main | For a session message: `queued`, `sending`, `paused`, then `received`, `skipped` or `failed` |

A session message can't be confirmed `delivered`: AX.25 only says the other station's radio
got it. Session messages don't need an `ack`.

## Build order

1. ✅ The AGW link and one session, both directions.
2. ✅ Several sessions: turn-taking, paging, limits, idle timeout, busy reply.
3. ✅ `RadioTests`: unit tests, and air tests with two users at once and a session opened
   from the connector.
4. Later: a "Connect to…" chat screen in RadioConnectorCli.

## Removing RC1

Connected mode replaces `RC1`, and `RC1`'s advantages over it (fewer turnarounds,
riding out weak and fading paths) mostly matter on HF, which this suite won't use. Until
it goes, `RC1` only gets fixes.

1. **Now:** connected mode on by default; the main program uses sessions.
2. **Test on real radios** with a few users: sessions, paging, busy and idle, links
   through a digipeater. Tune `maxSessions`, `idleMinutes` and `pageBytes` from what's seen.
3. **Remove `RC1` from RadioConnector:**
   - `Engine` and the air format in `connector.cpp`/`connector.h`, and its unit tests
   - `message.send` with `to`, `ack`, and the `RC1` fields of `message.status`,
     `message.received` and `radio.status` (`txQueue`, `inFlight`, `heldForMain`)
   - the `retry` and `holdForMainSeconds` config
   - the connector's KISS link, and the `RC1` air tests
   - `docs/rc1-test-messages.md`, and the `RC1` parts of `connector-protocol.md` and
     `json-format.md`
4. **Keep KISS everywhere else.** RadioConnectorCli is the testing and debugging program
   and keeps raw KISS as it is: its Receive/Transmit/Duplex packet screen, CQ, and the
   test tools all stay. `packet.cpp` (AX.25, KISS) stays for them and for RadioTests.

## Decisions

- Paging is done by the connector, not the main program.
- Sessions work both ways: incoming from any station, and outgoing with `session.open`.
- One line from the user is one prompt.
- Text to a session uses CR line ends, as packet terminals expect; `\n` and `\r\n` from
  the main program become `\r`.
- At most one window (4 frames) per session waiting in direwolf.
- Defaults: 4 sessions, 10 minutes idle, 1024-byte pages.
- `RC1` only reads UI frames, so a connected user can't inject `RC1` frames by typing them.
- Text for a session the connector is still opening waits in the connector: direwolf
  throws away data for a link it started until the other side answers.
