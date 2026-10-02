# Argus Example Client — Vision

A tiny command-line client that subscribes to the Argus RTSP Watcher MQTT feeds
and prints each message's **raw JSON** payload — one per line, on stdout — so it
pipes straight into `jq` or a file.

It exists to answer one question in code: *how do you consume the data a player
is publishing?* — and to be a copy-paste starting point for a real integration.

## What it does

- Connects to the player's MQTT broker.
- Subscribes to **every** analytics feed at once (`bs/argus/#`) — no hardcoded
  stream list; feeds are learned as data flows, and each message carries its own
  `device`/`stream` id.
- Prints each message's JSON payload verbatim on **stdout**; connection/status
  logs go to **stderr**, so stdout stays pure JSON.

## Why raw JSON

Downstream tooling (`jq`, a log file, another program) wants the data, not a
human summary. Keeping stdout to exactly the payloads means the client composes:

```sh
# pretty-print everything
argus-example-client --broker <player>:1883 | jq .

# only messages where someone is in a zone (see the main README's Zones section)
argus-example-client --broker <player>:1883 \
  | jq -c 'select(any(.tracks[]?; .in_zone == true))'
```

## Design

**Language:** Go (matches the repo; single static binary). It is a **separate Go
module** so its MQTT dependency does not leak into the stdlib-only watcher.

**Dependencies:** `github.com/eclipse/paho.mqtt.golang` for the MQTT client;
standard library for everything else. It does not parse the payload — it prints
the bytes as received, so schema changes never break it.

**Flow:** connect → subscribe to the topic (default `bs/argus/#`) →
`fmt.Println(payload)` per message → run until Ctrl-C (`--once` exits after the
first message).

**Flags:**

| Flag | Default | Meaning |
|------|---------|---------|
| `--broker` | (required) | MQTT broker `host:port`, e.g. `192.0.2.20:1883` |
| `--topic` | `bs/argus/#` | topic filter to subscribe to |
| `--once` | off | exit after the first message |

## Build & run

```sh
make build                       # -> bin/argus-example-client
./bin/argus-example-client --broker <player>:1883 | jq .
```
