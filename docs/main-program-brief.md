# Brief: the RtTC main program (local AI chat over packet radio and the network)

Use this whole file as the prompt for whoever (person or AI) designs and builds the main
program. It says what the program must do, what it must not do, which open-source parts
to build on, and where to read the RtTC protocol in full.

## Working rules

- **Ask, never assume.** When something about the setup isn't stated here (which machine,
  which version, how something is configured, what a user wants), ask before designing
  around it. If a guess would change what you build, it's a question. Open questions are
  listed at the end; ask them before the parts they affect.
- Ask design questions before starting any multi-file feature.
- Use industry-standard open-source software wherever it fits. The main program itself
  should do as little as possible: glue, not infrastructure.
- No AI attribution or co-author trailers in commits or PRs.
- Everything runs locally. **No cloud services and no cloud APIs**, not even as a fallback.

## What it is

RtTC (RTTY-Connector) is an existing C++ program that connects a VHF/UHF packet radio
(direwolf, AX.25 connected mode with FX.25) to a **main program** over JSON/TCP. People on
the radio connect to the station's callsign like a packet BBS and type lines; the main
program answers them.

The new project is that main program. It:

1. Accepts users from **RtTC connectors** (one or more radios) and from **the network**.
2. Sends their prompts to **local AI nodes**: dedicated Linux machines running LLMs.
3. Keeps users, conversation history, prompt and trace logs.
4. Sends the answers back the way each user came in.

## Decided

| Topic | Decision |
|---|---|
| Main program language | Python (asyncio). RtTC stays C++ |
| OS | Linux on every machine: radio hosts, core host, AI nodes |
| Packaging | Docker for everything, RtTC included. Docker Compose on the core host |
| RtTC in Docker | Must see the sound card (e.g. AIOC USB cable) and serial ports for PTT/CAT: pass `/dev/snd`, `/dev/ttyUSB*`/`/dev/ttyACM*`, groups `audio` and `dialout`. direwolf runs inside the same container |
| Bluetooth radios | Probably not used. Don't design the containers around Bluetooth |
| Machines | The main program may share a machine with a radio host. Must also work on separate machines (everything talks over TCP) |
| AI nodes | Software not chosen yet. The main program must not depend on which one: it only speaks the OpenAI-compatible `/v1/chat/completions` API, through a gateway |
| Gateway | LiteLLM Proxy between the main program and the nodes: routing, load balancing, failover, per-user limits, logging. Adding or swapping a node is a config change, not a code change |
| History and users | PostgreSQL |
| Observability | Langfuse (self-hosted) from day one: traces, prompt versions, latency. Note Langfuse v3 needs ClickHouse, Redis and S3-compatible storage (MinIO) alongside it |
| RF vs network users | **Separate.** Different user records, different histories, different profiles, even for the same callsign or person |

## Architecture

```
 Radio host (Linux, docker)                Network users
 ┌───────────────────────────┐             (client type: open question)
 │ direwolf ─AGW─ RtTC        │                    │
 │ (/dev/snd, /dev/ttyUSB*)  │                    │
 └───────────┬───────────────┘                    │
             │ JSON/TCP, connector dials out       │
 ┌───────────▼─────────────────────────────────────▼──────────┐
 │ Core host (Linux, docker compose)                           │
 │   main (Python) ──► LiteLLM ──► AI nodes (OpenAI API, LAN)  │
 │        │               │                                    │
 │        └── PostgreSQL ◄┘        Langfuse (+ its services)   │
 └─────────────────────────────────────────────────────────────┘
```

### Main program modules (suggested, not fixed)

| Module | Job |
|---|---|
| `transports/rttc.py` | asyncio TCP server for RtTC connectors. Speaks the JSON protocol below |
| `transports/<network>.py` | The network client transport (type still to decide) |
| `sessions.py` | One session model no matter where the user came from |
| `users.py` | RF identity (callsign) and network identity, kept separate |
| `history.py` | Postgres: load and save turns |
| `llm.py` | OpenAI SDK pointed at LiteLLM; model chosen by profile name, e.g. `chat-fast` |
| `profiles.py` | Per-source rules: system prompt, model name, length limits, charset |
| `filters.py` | Hooks run on an answer before it's sent (trim, charset; a safety check is an open question) |

Adding a new way in (another transport) should be one new file.

### Message flow (RF)

```
station types a line
 → RtTC        message.received {session, from, text}
 → main        find user (callsign) and conversation, load last N turns
 → main        profile "rf": short answers, plain ASCII
 → LiteLLM     chat completion on the profile's model name → least-busy node
 → main        filters, save both turns, trace to Langfuse
 → RtTC        message.send {session, text, ref}
 → RtTC        pages it out; message.status reports delivery
```

## The RtTC protocol

Full references, in the RtTC repository (`https://github.com/DomAtTheShack/RtTC`, branch
`master`; locally `C:\Users\dooli\CLionProjects\RTTY-Connector` on the Windows dev machine):

| File | What's in it |
|---|---|
| `docs/json-format.md` | **Every message and field.** The authority: read it before writing the transport |
| `docs/connector-protocol.md` | Overview: connection, envelope, config, design choices |
| `docs/connected-mode.md` | How sessions, paging, turn-taking, limits and idle timeouts work |
| `docs/radios.md` | Radios and cables (AIOC etc.), PTT types |
| `src/Connector/radio_connector.cpp` | The connector program: request handling |
| `src/Connector/sessions.cpp`, `sessions.h` | Session state, paging, `M`/`N`/`B` handling |
| `src/Connector/connector_config.cpp` | Config fields and validation |
| `src/Connector/connector.cpp` | The JSON envelope |
| `src/Connector/agw.cpp` | direwolf's AGW port |
| `direwolf/` | direwolf source, built with RtTC |
| `CMakeLists.txt` | The build (needed for the RtTC Dockerfile) |

If anything below disagrees with `docs/json-format.md`, that file wins. Read the source when
the docs don't answer something.

### Summary

**Connection.** The main program **listens** on TCP (e.g. `:9000`); each RtTC connector
**dials out** to it (`RadioConnector --main host:9000`, saved in its config) and retries
every 5 s. One JSON object per line, UTF-8, `\n`-terminated, at most 64 KiB. Several
connectors may connect at once: tell them apart by `hello.body.instance` (the station's
callsign).

**Envelope**, both directions:

```json
{"v":1, "type":"message.send", "id":"m-102", "re":null, "body":{ }}
```

`id` on any request you want a reply to; replies carry it back as `re`. Errors are a reply:
`{"type":"error","re":"m-102","body":{"code":"bad_field","field":"...","message":"..."}}`.

**Main → connector**

| `type` | `body` | Reply |
|---|---|---|
| `message.send` | `{"session","text","ref"?}` | `message.accepted` `{msgId, session, pages}` |
| `session.open` | `{"to","path"?}` | `session.opening` `{session, peer}` |
| `session.close` | `{"session","text"?}` | `session.closing` |
| `session.list` | `{}` | `sessions` |
| `config.get` / `config.set` | `{}` / `{"config":{...}}` | `config` `{config, restartRequired}` |
| `audio.list` | `{}` | `audio.devices` |
| `status.get` / `radio.restart` | `{}` | `radio.status` |

**Connector → main**

| `type` | When |
|---|---|
| `hello` | On every connect: `{program, version, instance, capabilities, config, radio}` |
| `session.opened` | A session is open: `{session, peer, direction:"in"/"out"}`. **Re-sent for every open session on each reconnect** |
| `message.received` | One line typed: `{msgId, session, from, to, text, firstHeard, completed}` |
| `session.new` | The user typed `N`: forget the conversation and start a new one |
| `message.status` | Delivery of a reply: `queued`, `sending`, `paused`, then final `received`, `skipped` or `failed` (+`reason`). Carries your `ref` |
| `session.closed` | `{session, peer, reason}`: `bye`, `closed`, `remote`, `idle`, `link_lost`, `no_answer`, `replaced`, `radio_down`, `disabled`, `restart` |
| `radio.status` | On any change: `{state, direwolf, ptt, sessions, lastHeard, error}` |

On connect the order is: `hello`, a `session.opened` per open session, `radio.status`.

### What this means for the main program

- **Session IDs** (`s-4`) are unique only within one connector run. Key sessions by
  `(instance, session)`, and expect IDs to repeat after a connector restart.
- **Reconnects** re-send `session.opened` for sessions the main program may already know:
  handle them idempotently.
- **`message.send` and `session.open` work only while `radio.status.state` is `ready`.**
  Otherwise you get `radio_not_ready`.
- **Paging is the connector's job.** Long answers are split at `connected.pageBytes`
  (default 1024) and the user types `M` for more. Don't page in the main program.
- **`M`, `N`, `B` never reach the main program as prompts.** `N` arrives as `session.new`.
  Any other line typed while a reply is paging drops the rest of it (`skipped`) and arrives
  as the next `message.received`.
- **Record delivery in history.** A reply that ended `skipped` or `failed` wasn't fully
  read; store its final `message.status` with the turn (use `ref` to link them).
- **RF is slow.** 1200 baud, about 180-byte frames, one half-duplex channel shared by up to
  `maxSessions` users (default 4). The RF profile must ask the model for short, plain-ASCII
  answers. The connector turns `\n` into CR for packet terminals.
- **No acknowledgement** of `message.received` is needed or possible.
- **If the main program is down**, the connector tells stations "The repeater's program is
  offline" itself.
- **The main program owns the connector's config** (`config.get` / `config.set`). An admin
  view of each connector's config and `radio.status` is useful but not first.
- The connector never sets frequency or mode, and it's VHF/UHF only (never HF).

## Deliverables

1. **RtTC Dockerfile** (Linux) that builds RtTC and its bundled direwolf, and a compose
   service for a radio host with the sound card and serial ports passed through.
   The C++ connector itself shouldn't need changes; ask before changing it.
2. **The main program** (Python, asyncio) with the RtTC transport first, then the network
   transport.
3. **Compose stack for the core host**: main, LiteLLM (with an example `config.yaml` for
   several nodes under one model name), PostgreSQL, Langfuse and its services.
4. **Database schema**: users (RF and network separate), conversations, messages, delivery
   state.
5. **Example AI node setup** for one node, without locking in the node software.
6. **Tests**: the RtTC transport against recorded JSON exchanges (`docs/json-format.md`
   has a full example session), and a fake connector for end-to-end tests without a radio.
7. **Docs**: how to deploy on one machine and on several, and how to add an AI node.

## Open questions (ask before the parts they affect)

1. **Network users:** how do they connect: telnet (BBS-style), a web page, websocket, or
   something else? Which first?
2. **Network identity:** do network users log in? If so, how (local accounts, Keycloak or
   Authentik over OIDC)?
3. **RF identity:** is a callsign alone trusted as the user, or is anything checked?
4. **RF history length:** does an RF user's history carry over between connections, or
   only last until they disconnect or type `N`?
5. **A new prompt while the model is still answering** the same session: queue it, cancel
   the running request, or ignore it?
6. **Content filtering before anything is transmitted** (the licensee is responsible for
   what goes out on the air): wanted? A safety model (e.g. Llama Guard) on a node, a word
   list, or none?
7. **Models:** one model name for everything, or tiers (fast/big) chosen by profile or by
   the user?
8. **Scale:** how many connectors, RF users and network users at once?
9. **Retention:** how long are history, logs and Langfuse traces kept?
10. **Repository:** a new repo for the main program, or inside RtTC? Where does the RtTC
    Dockerfile live?
