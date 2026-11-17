#!/bin/sh
# RadioConnector, dialing MUS_ADDR (IPv4:port) when it's set; any arguments are passed on.
# To run something else in the container (e.g. RadioConnectorCli, aplay -l), give it as
# the command: docker compose run --rm rttc RadioConnectorCli
set -e
case "$1" in
    ""|-*)
        if [ -n "$MUS_ADDR" ]; then
            set -- --main "$MUS_ADDR" "$@"
        fi
        exec RadioConnector "$@"
        ;;
    *)
        exec "$@"
        ;;
esac
