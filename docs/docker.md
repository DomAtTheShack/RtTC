# Docker (Linux radio host)

`docker/Dockerfile` builds RadioConnector and RadioConnectorCli, with direwolf and Hamlib
built from source (the versions and checksums the CI uses). direwolf runs inside the same
container as RadioConnector. `docker/compose.radio.yaml` runs it with the radio's sound
card and serial ports passed in. Bluetooth audio is left out of the image.

## Run

```sh
cd docker
cp radio.env.example radio.env
$EDITOR radio.env            # MUS_ADDR, AUDIO_GID, DIALOUT_GID
docker compose -f compose.radio.yaml --env-file radio.env up -d --build
docker compose -f compose.radio.yaml logs -f
```

- `MUS_ADDR` is where the main program listens, as an IPv4 address and port.
  `127.0.0.1:9000` when MUS runs on the same machine (the container uses host
  networking). It's passed as `--main` on every start.
- `AUDIO_GID` / `DIALOUT_GID` are the host's group IDs for `/dev/snd/*` and the serial
  ports: `stat -c %g /dev/snd/controlC0` and `stat -c %g /dev/ttyACM0`.

## Devices

The container sees the host's `/dev`, but may only open sound cards (`/dev/snd`, major
116) and USB serial ports (`/dev/ttyUSB*` 188, `/dev/ttyACM*` 166). That way an AIOC
cable or USB serial adapter that's unplugged and comes back under another name still
works without restarting the container. `/sys` is visible as usual, which is how the AIOC
cable is found.

A radio on another kind of serial port (e.g. `/dev/ttyS0`, major 4) needs its own rule in
`device_cgroup_rules`.

Check what the container sees:

```sh
docker compose -f compose.radio.yaml run --rm rttc aplay -l
docker compose -f compose.radio.yaml run --rm rttc sh -c 'ls -l /dev/ttyUSB* /dev/ttyACM*'
```

## Settings

They're kept in the `rttc-data` volume (`/data/rttc/connector.ini`), so they survive
rebuilds. The main program owns them through `config.get` / `config.set`
([json-format.md](json-format.md#config)): until a callsign and audio are set, the radio
stays `unconfigured` but still connects and says `hello`.

To set them by hand instead, stop the service and edit the file in the volume:

```sh
docker compose -f compose.radio.yaml run --rm rttc sh -c 'cat /data/rttc/connector.ini'
```

## Build only

```sh
docker build -f docker/Dockerfile -t rttc .
```

The `Docker` workflow builds this image on every PR that touches the build, checks the
programs and their libraries, and checks that RadioConnector dials a main program and
says `hello`.
