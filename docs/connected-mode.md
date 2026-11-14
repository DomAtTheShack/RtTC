# Connected mode

Connected mode is how people reach the connector, and it's on by default
(`connected.enabled`). The suite is for VHF/UHF only, never HF.

An ordinary packet station (a TNC, a terminal program, an app) talks to the connector
without our software: it types `connect N0CALL-7` and gets a session, like a packet BBS.
The main program, e.g. an LLM chat, answers through JSON
([connector-protocol.md](connector-protocol.md)). It works both ways: the main program can
also open a session to another station.

```
 packet terminal ──AX.25 connected── direwolf ──AGW── connector ──JSON── main program
 (any station)       (SABM, I, RR)    (link layer)      (sessions)        (LLM chat)
```

## How it works

direwolf runs the AX.25 link itself (connect, sequence numbers, acks, retries) and the
connector drives it over direwolf's **AGW** port. The connector only sees connects,
disconnects and data; `RadioConnector --verbose` logs every one of them.

RadioConnectorCli is the testing and debugging program. Its Duplex mode works with raw
UI packets over direwolf's KISS port, and also shows the text of a session's `I` frames,
but not the control frames (`SABM`, `UA`, `RR`, `DISC`). Its
[Connected mode](#radioconnectorcli) is a connected-mode station of its own.

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
| `message.send` | main → connector | `{"session":"s-3","text":"..."}`. Reply `message.accepted` |
| `message.status` | connector → main | For a session message: `queued`, `sending`, `paused`, then `received`, `skipped` or `failed` |

Text sent can't be confirmed read: AX.25 only says the other station's radio got it.

## Build order

1. ✅ The AGW link and one session, both directions.
2. ✅ Several sessions: turn-taking, paging, limits, idle timeout, busy reply.
3. ✅ `RadioTests`: unit tests, and air tests with two users at once and a session opened
   from the connector.
4. ✅ A connected-mode chat screen in RadioConnectorCli ([below](#radioconnectorcli)).

## RadioConnectorCli

**Settings > Mode > Connected** turns RadioConnectorCli into a connected-mode station, for
testing by hand without the main program. It runs the same session code as RadioConnector
(`src/Connector/sessions.*`, through direwolf's AGW port), playing the main program's part
itself: what the main program would get as JSON is shown as plain text, and typed lines go
out as `message.send`.

Settings for it:

| Setting | |
|---|---|
| **Connect to** | Who to connect to at the start, e.g. a RadioConnector's `N0CALL-1`. Empty = wait for calls. It's asked again each Start |
| **Frequency** | Tuned over CAT (Settings > Radio (CAT)) at the start. Empty = leave the radio |

While the screen is open, other stations can connect to this one's callsign (up to
`max_sessions` in `settings.ini`, default 4). Sessions don't time out and aren't paged.
Typed lines go to the session shown before the `>`. Commands:

| Command | |
|---|---|
| `/c CALL [via D1,D2]` | Connect, through up to 2 digipeaters |
| `/d [CALL]` | Disconnect (the current session if no `CALL`) |
| `/s [CALL]` | List the sessions, or switch to `CALL`'s |
| `/r [N] [SECS]` | Range test: `N` numbered lines (default 10), each `SECS` (default 5) after the last one's echo |
| `/r stop` | Stop the range test |
| `/l` | The link log: connects, disconnects, each frame's audio level and every problem below, with times |
| `/rx [+\|-\|N]` | Bluetooth radio: receive gain in % (10-400, `+`/`-` step 10). Saved |
| `/tx [+\|-\|N]` | Bluetooth radio: transmit gain in % (10-200). Saved |
| `/h` | Help |

Esc disconnects every session, then stops; Esc again stops now.

- **To a RadioConnector**: type prompts and read the main program's replies. Long replies
  stop at `[M]ore, [N]ew, [B]ye`; type `M`, `N` or `B`.
- **To another RadioConnectorCli** in Connected mode: plain chat both ways. A line typed as
  just `B`, `M` or `N` is read as the command it is to RadioConnector (`B` disconnects).
- **Range test**: the far end must be RadioConnectorCli in Connected mode. It echoes each
  `RANGE n/N` line back as `ECHO RANGE n/N`, and the sender shows each round trip and, at
  the end, how many were sent, echoed and lost (no echo within 120 s), with the shortest,
  longest and average round trip. AX.25 resends lost frames itself, so a weak path shows
  up as longer round trips, and a path that's too weak as `link lost`.

**Audio levels**: before the prompt, `[CALL level 48 (14/12) ok]` is how loud the last
frame from the station being talked to (or the last station heard) was here, as direwolf
measures it. About 50 is best: green 30-70, yellow 20-29 or 71-90, red outside that. The
numbers in brackets are the two tones' levels. When a station's level gets worse, a line
says which way to turn what. The level here depends on the sender's transmit level and
this station's receive level:

- **Bluetooth radio**: `/rx` changes what this station hears, `/tx` what the other station
  hears (watch their level for us on their screen). Both take effect at once and are saved
  (`bt_rx_gain`, `bt_tx_gain` in `settings.ini`).
- **Sound card** (e.g. an AIOC cable): the sound card's recording level (receive) and
  playback level (transmit) in the computer's sound settings, and the radio's volume.

**Another direwolf** already running when a session starts is asked about: on the same
radio, both answer every frame and keep resetting each other's links. (RadioConnector logs
a warning instead.)

**Link problems** show as dim `! HH:MM:SS` lines where they happen, in any session, and
`/l` lists them all. They're things that don't happen on a healthy link:

| Shown | Meaning |
|---|---|
| No answer from CALL in time: we asked again, and they answered | Our frame or their answer was lost; direwolf polled them (their answer had the F bit) |
| No answer from CALL for 10 s (N frames not acknowledged) | Our frames are waiting and nothing is coming back |
| CALL got no answer from us in time and asked again | Their timer ran out (they polled us): our answer didn't reach them |
| CALL sent the same frame again: our acknowledgement didn't reach them | A resend of a frame we already had |
| CALL missed a frame from us and asked for it again (REJ/SREJ) | One of our frames didn't get through |
| CALL is busy and asked us to wait (RNR) | They can't take more yet |
| CALL asked to connect again on an open link | Our answer to their connect was lost, or they restarted |
| CALL refused the connection / says the link isn't connected (DM) | |
| CALL rejected a frame as invalid (FRMR) | A protocol error |
| Disconnected (no answer / link lost / direwolf stopped), not delivered, no echo | Also marked `!` in `/l` |

A poll after a minute or more of quiet is direwolf checking the link is still there (its
T3 timer, every 5 minutes by default), not a problem: it goes in `/l` unmarked. Ours
counts only with nothing of ours waiting.

direwolf only reports frames it hears, not its own connected-mode ones, so our own resends
show up through the other station's answers (the F bit) or as no answer at all.

## Next

Test on real radios with a few users: sessions, paging, busy and idle, links through a
digipeater. Tune `maxSessions`, `idleMinutes` and `pageBytes` from what's seen.

`RC1`, the earlier UI-frame protocol between connectors, was removed in favour of
connected mode. RadioConnectorCli keeps raw KISS for testing: its Duplex packet screen,
CQ, and the test tools. `packet.cpp` (AX.25, KISS) stays for them and for RadioTests.

## Decisions

- Paging is done by the connector, not the main program.
- Sessions work both ways: incoming from any station, and outgoing with `session.open`.
- One line from the user is one prompt.
- Text to a session uses CR line ends, as packet terminals expect; `\n` and `\r\n` from
  the main program become `\r`.
- At most one window (4 frames) per session waiting in direwolf.
- Defaults: 4 sessions, 10 minutes idle, 1024-byte pages.
- Text for a session the connector is still opening waits in the connector: direwolf
  throws away data for a link it started until the other side answers.
