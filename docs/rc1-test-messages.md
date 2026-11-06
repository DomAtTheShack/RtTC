# RC1 test messages for the menu program

Type these lines in the menu program (RadioConnectorCli) to test the Radio Connector's ACKs
over the air, with no main program needed. How the ACKs work is in [ack-guide.md](ack-guide.md).

## Setup

The menu program and the Radio Connector are two different stations: the menu uses
`settings.ini` (e.g. `N0CALL`) and the connector uses `connector.ini` (e.g. `N0CALL-1`). Both share
the sound card or VB-Cable, so each hears the other.

1. Start the connector: `RadioConnector --verbose`. On its first run it creates `connector.ini`
   as a copy of `settings.ini`, with its own callsign. Its `settings:` log line says which
   callsign it uses. To use another file, add `--settings <file>`.
2. Point it at a port nothing else uses, once: `RadioConnector --main 127.0.0.1:9000 --verbose`.
   It saves the address in `connector.ini`. (8080 is a common web-server port; anything listening
   there accepts the connection but never acks, so every message ends in `EXP`.)
3. In the menu program, open Settings and set **Mode** to Duplex, so you can type and also see
   the replies.
4. Set **Send to** to the connector's callsign (`N0CALL-1`). The session header then shows
   `Sending to N0CALL-1`. Leave it empty to go back to CQ.
5. Start the session, type a test line from the table below, and press Enter. Each line you
   type becomes one packet.

No main program is needed. Without one, the connector still sends its ACK, but it holds the
message for 180 s and then sends `EXP` instead of `OK`. With nothing on the `main=` port it logs
"can't reach the main program" every 5 s, which is fine.

## Message format

Every field is split by `|`, and the text is always last.

| You type (data part) | Field | Rule |
| --- | --- | --- |
| `RC1` | Tag | Exactly `RC1`, or the frame is ignored |
| `D` | Kind | `D` = data. You only ever type `D` |
| `001` | Message ID | 3 hex digits, `000`–`FFF`. Use a new one per test |
| `2/3` | Part / total | Part 1–total, total 1–40. Every part of one message must give the same total |
| `text` | Text | Anything; may contain `\|`. Parts are joined with nothing in between, so end a part with a space if you want one |

| Comes back (shown as `N0CALL-1>N0CALL:`) | Meaning |
| --- | --- |
| `RC1\|A\|002\|1,3` | ACK: has parts 1 and 3 of message 002 |
| `RC1\|A\|002\|1-3` | ACK: has parts 1 to 3 |
| `RC1\|R\|002\|OK` | Result: its main program took the message |
| `RC1\|R\|002\|EXP` | Result: no main program took it within 180 s |
| `RC1\|R\|002\|ERR` | Result: the message was bad (parts disagree on the total, or not UTF-8) |

## Test messages

Type each line and press Enter. Where a test has two lines, type the first, press Enter, then
type the second. Typing takes longer than the connector's 2 s wait, so you'll also see an ACK
for the parts so far after the first line (e.g. `RC1|A|002|1`). That's expected. Run them in
order: tests 4 and 12 build on earlier ones.

| # | Test | Type | Should come back |
| --- | --- | --- | --- |
| 1 | One part | `RC1\|D\|001\|1/1\|Hello` | Right away: `RC1\|A\|001\|1`. With no main program, 180 s later: `RC1\|R\|001\|EXP` |
| 2 | Two parts | `RC1\|D\|002\|1/2\|Hello, ` then `RC1\|D\|002\|2/2\|part two` | `RC1\|A\|002\|1-2` as soon as part 2 is heard |
| 3 | Missing part | `RC1\|D\|003\|1/3\|one ` then `RC1\|D\|003\|3/3\|three` | 2 s after the last part: `RC1\|A\|003\|1,3` (part 2 missing) |
| 3b | Fill the gap | `RC1\|D\|003\|2/3\|two ` | Right away: `RC1\|A\|003\|1-3`. The connector joins it into "one two three" |
| 4 | Repeat part | `RC1\|D\|003\|2/3\|two ` again | 2 s later: `RC1\|A\|003\|1-3` again, plus the result if it has one. The main program doesn't get it twice |
| 5 | Totals disagree | `RC1\|D\|004\|1/2\|a` then `RC1\|D\|004\|1/3\|b` | Right away: `RC1\|R\|004\|ERR` |
| 6 | Half a message | `RC1\|D\|005\|1/2\|only half` | 2 s later: `RC1\|A\|005\|1`. After 2 min with no part 2, the connector drops it silently |
| 7 | Not RC1 | `hello` | Nothing. The connector only logs `rx` |
| 8 | Unknown kind | `RC1\|X\|bad` | Nothing |
| 9 | Bad ID | `RC1\|D\|0G1\|1/1\|x` | Nothing (`G` isn't hex) |
| 10 | Part past total | `RC1\|D\|006\|3/2\|x` | Nothing |
| 11 | Wrong station | Set Send to empty (CQ), then `RC1\|D\|007\|1/1\|x` | Nothing: it isn't addressed to N0CALL-1 |
| 12 | Reused ID | Within 10 min of test 1 finishing: `RC1\|D\|001\|1/1\|Hello again` | The ACK and result again, but the new text is not delivered: it counts as a repeat of message 001 |

## Reading the results

In the menu program, replies show as `N0CALL-1>N0CALL: RC1|A|…`. The `>N0CALL` part appears whenever
a packet isn't addressed to CQ, so you can tell an ACK from ordinary traffic. Your own sent lines
show as `N0CALL>N0CALL-1:`.

In the connector's `--verbose` window, a working test reads like this (from a run on
2026-10-29, with a stand-in main program that acked):

```
12:08:27 rx N0CALL>N0CALL-1: RC1|D|002|1/3|one
12:08:28 rx N0CALL>N0CALL-1: RC1|D|002|3/3|three
12:08:30 tx N0CALL-1>N0CALL: RC1|A|002|1,3
12:08:38 rx N0CALL>N0CALL-1: RC1|D|002|2/3|two
12:08:38 tx N0CALL-1>N0CALL: RC1|A|002|1-3
12:08:38 to main: {"type":"message.received", ... "text":"one two three"}
12:08:38 from main: {"type":"ack","re":"c-1"}
12:08:38 tx N0CALL-1>N0CALL: RC1|R|002|OK
```

| Line | Meaning |
| --- | --- |
| `rx A>B: …` | Heard a frame from A to B (logged before it's processed) |
| `tx A>B: …` | The connector sent a frame |
| `to main: …` | JSON sent to the main program (only while one is connected) |
| `from main: …` | JSON from the main program |
| `direwolf: …` | direwolf's own output, only with `--direwolf`. It can show up a few seconds late |

Tests 2, 3, 3b, 4, 8 and the full OK result were run that way over VB-Cable. The rest follow from
the code (`Engine::onFrame` and `Engine::tick` in `src/Connector/connector.cpp`).

**Known quirks**

- `--verbose` shows only the connector's own lines. Add `--direwolf` to also see direwolf's output
  (startup, device list, `[0.3] …` for frames heard, `[0L] …` for frames sent).
- Anything typed in the menu program goes out over the air, even a command typed into the wrong
  window. The connector ignores it unless it's a valid `RC1` frame.
- If direwolf reports "Audio input level is too high" (it wants about 50), turn the output device
  down, e.g. CABLE Input to about 25% in Windows Sound settings. It still decodes, but it's
  clipping.
- The other direction (the connector sending) needs a main program to send `message.send`.
  `RadioTests` does that automatically; see the `air` group in the README.
