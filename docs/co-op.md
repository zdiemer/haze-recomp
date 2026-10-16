# Experimental campaign co-op

Campaign co-op uses invite codes, direct WebRTC peer connections and TURN relay
fallback. It does not use PSN, Ubisoft accounts or friend lists. Each player must
run a compatible native build and have their own prepared BLUS30094 game files.

There is no hosted service configured by this repository. Enter your deployed
server's address in **Server address** on either co-op screen. A hostname such
as `coop.example.com` or `coop.example.com:8443` uses secure WebSockets and the
`/v1` endpoint automatically. A full `wss://YOUR-SERVICE/v1` URL also works.
The [rendezvous service guide](../services/rendezvous/README.md) explains deployment
with TLS and a TURN server. The game remembers the server in `co-op-server.txt`
under its save-data root, separately from campaign saves. The old
`HAZE_COOP_SERVICE` environment variable is no longer used. Do not share a TURN
secret with players. Certificate verification uses Windows trust roots; a
private CA can be supplied with `HAZE_COOP_CA_FILE`.

## Host and join

1. The host selects **Campaign → Host Co-op → Server address**, types or pastes
   the address, and selects **Save server**, then **Create invite code**.
2. Copy the code and share it **and the server address** with the other players. Select **Continue**, then
   continue a campaign or choose a chapter.
3. Each joining player selects **Campaign → Join Co-op**, saves the same
   **Server address**, then selects **Enter invite code**.
   Type or paste the code with Ctrl+V. Controller users can also add or delete
   characters using the rows below the connection status.
4. Select **Connect to host**, wait for a connected status, then **Continue**.
   Select the host's game in the game list to join the campaign.

Codes are case-insensitive; spaces and hyphens are ignored. A code expires when
the host leaves its room or the service restarts. Back from the invite page
closes an unfinished session. Disconnect releases that session and allows a
new invite. Continue preserves the connection while opening the game's campaign
or game-list screen. A host departure ends the other players' campaign session.

Server entry uses the keyboard, with Ctrl+V to paste and Ctrl+A to clear the
field before replacing it. Enter or **Save server** saves it immediately;
Cancel or losing focus discards the edit. Saving a different server disconnects
the current co-op session. Invite codes belong to the server that created them.

For LAN play, choose **Local network** on the host and join pages, then Continue.
For single player, choose **Single Player Campaign**; no co-op service is needed.

The **Player Profile** entry on the main menu changes the displayed name and
persists it across restarts. Names contain 3–16 ASCII letters, digits, `_` or `-`,
starting with a letter. Existing saves keep their original internal identity.
The menu header, server list and campaign teammate label use the chosen name.

## Validation and limits

Two local game instances joined through the native invite pages, loaded the
same campaign, and synchronized movement. The host survived client departure,
a fresh client rejoined the same invite, and host closure produced the game's
lost-connection dialog. A separate run with `PS3_NET_ONLINE=0` loaded the
single-player checkpoint without a co-op service. A subsequent two-instance
campaign run verified each player's chosen name above the other character,
with movement still synchronized. Copied save profiles remained unchanged.

Transport tests cover four direct peers and two peers using a selected TURN
relay route. Secure signaling tests accept a trusted certificate and reject
an unrelated CA, an untrusted certificate and a hostname mismatch.

Separate Windows machines, home routers, four-player gameplay and four-peer
relay operation remain unvalidated. The Windows VM test is deferred. Networks
that block all UDP require additional TCP/TLS relay support. Competitive modes,
voice chat, browser integration and full-campaign completion are outside the
validated co-op behavior. See the [implementation record](native-network-investigation.md).
