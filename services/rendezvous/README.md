# Haze co-op rendezvous

The native client uses this service for invite codes and WebRTC signaling. Game
datagrams travel directly between peers when ICE finds a route, with TURN as a
fallback. No PSN account, Ubisoft login, or friend invitation is involved.

This service is not deployed by the repository. Players enter its address in
**Server address** on the in-game Host Co-op or Join Co-op screen. A hostname
(optionally with `:port`) defaults to `wss://YOUR-SERVICE/v1`; a full WSS URL
can specify a different proxy path. Everyone in a room must use the same server.
The game saves the address between launches. TLS certificates are verified against
Windows trusted roots; `HAZE_COOP_CA_FILE` can supply a private CA PEM file.
Never put the TURN shared secret in a game build or client configuration.

## Local development

Install `requirements.txt` into a virtual environment, then run:

```powershell
$env:HAZE_LOCAL_TEST = '1'
python services/rendezvous/server.py
```

For native game tests, set `HAZE_COOP_LOCAL_TEST=1` and enter
`ws://127.0.0.1:8765/v1` in the game's **Server address** field.
Insecure WebSocket connections are accepted only for
that explicit loopback test mode. Each game instance needs a separate save root.

Build `port/net` independently with CMake to obtain `test_network_session` and
`network_relay`. No game files are needed for these transport tests:

```text
python services/rendezvous/test_server.py
python services/rendezvous/test_transport.py build/network
python services/rendezvous/test_signaling_tls.py build/network
```

The tests cover room isolation, malformed requests, capacity, expired codes,
four direct peers, binary datagrams, departure/rejoin, host closure, and a
two-peer TURN route. The TURN fixture binds only loopback and uses test-only
credentials. It is not a public relay implementation. Four-peer relay and
separate-machine/router tests remain to be completed. A relay-only candidate
policy can still produce peer-reflexive routes when testing on one machine;
the tests inspect selected candidate pairs rather than assuming TURN was used.
The TLS test requires OpenSSL on PATH and generates temporary test keys. It
verifies the trusted-CA path and rejection of unrelated trust, system-untrusted
certificates and hostname mismatch. `port/net/patch_verified_tls.py` produces a
guarded build-local copy of libdatachannel's WebSocket source to keep its
MbedTLS certificate verifier enabled on Windows. The fetched source is unchanged.

## Deployment

Run `server.py` under a process supervisor behind a TLS WebSocket reverse proxy.
The default listener is `127.0.0.1:8765`; configure `HAZE_SIGNAL_BIND` and
`HAZE_SIGNAL_PORT` only when needed. Forward `/v1` and preserve WebSocket upgrade
headers. Apply connection and request limits per client IP at the proxy. The
application doesn't trust forwarded IP headers, so connections through one
proxy also share its conservative application connection limit (16).

Provide a TURN server with a public address and reachable UDP listening/relay
ports. Set `HAZE_TURN_HOST` to its DNS name and `HAZE_TURN_SECRET` to the same
secret configured for its REST authentication. The service generates expiring
credentials using the coturn-compatible timestamp/HMAC mechanism described in
the [coturn documentation](https://github.com/coturn/coturn/blob/master/README.turnserver).
Restrict relay permissions to public peer destinations and configure bandwidth,
allocation, and total-user quotas. Do not enable anonymous relay access or
loopback peers on a public service.

Room codes grant entry to a room. Anyone with the code can occupy an available
slot. Codes use 12 random characters, are never listed publicly, and expire when
the host disconnects. The current service has no account system or persistence;
a restart ends all rooms. TURN credentials currently last 24 hours and aren't
renewed during a session. This implementation advertises UDP TURN; networks
blocking all UDP need additional TCP/TLS relay integration and validation.

## Shared protocol: version 1

Connect a WebSocket to `/v1`. The first JSON message must arrive within ten
seconds and include `v: 1`, `game: "haze-blus30094-v1"`, and `op: "create"` or
`op: "join"`. Join also includes the normalized 12-character `code`.

The server returns `joined` with `v`, `id` (1–4), `code`, existing `peers`, and
`ice` server objects (`urls`, optional `username` and `credential`). ID 1 owns
the room. Existing members receive `peer` with the new ID. Every pair creates a
WebRTC connection; the lower ID offers. Forward an SDP with:

```json
{"op":"signal","to":2,"description":{"type":"offer","sdp":"..."}}
```

Or a trickled candidate with:

```json
{"op":"signal","to":2,"candidate":{"candidate":"candidate:...","mid":"0"}}
```

Recipients get `from` instead of `to`. The server supplies that sender identity
and only forwards within the same room. `left` removes one peer; `closed` ends
the room. `error` contains a stable `code`: `version`, `rate_limit`, `busy`,
`invalid_code`, `not_found`, `full`, `protocol`, or `peer_gone`.

The offerer creates a data channel named `haze-udp-v1`, protocol `haze/1`,
unordered, with zero retransmissions. Every binary message is exactly one game
UDP payload, up to 65,507 bytes. Text messages carry no game data. Peers map room
IDs to virtual IPv4 addresses `10.46.0.ID`, port 30000, and forward broadcasts
to all connected room members. The channel's authenticated connection supplies
the sender ID; packets cannot choose another sender. Bounded queues drop game
packets under congestion, preserving the game's own UDP retry semantics.

Browser issue #2 can implement this same signaling and data-channel contract.
The browser guest/socket integration is separate and has not been validated.
