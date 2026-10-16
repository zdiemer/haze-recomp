# Native multiplayer investigation

This is an implementation record for #46 and the native side of #2. Internet
invite transport and native host/join menus are implemented. Two local instances
have joined the same campaign through invite codes and visibly synchronized
movement. Cross-machine/router acceptance remains deferred.

## Retail path

The BLUS30094 executable imports the older `sceNp` authentication, ticket,
basic and signaling interfaces. It does not import Matching2. The SDK's
Matching2/PSNR implementation therefore does not establish Haze compatibility.
The executable also contains DemonWare lobby component names and the Ubisoft
authentication, lobby and STUN endpoints. A successful fake PSN login alone
would not replace those game services.

The independent LAN path is accessible through Campaign → Host Co-op → LAN
and Campaign → Join Co-op → LAN. A host can continue an existing checkpoint.
Both the host and the discovery client use an ordinary IPv4 UDP socket bound
to port 30000, rather than the SDK's special P2P socket types. Consequently
`PS3_NET_P2P_PORT` does not remap these sockets.

## Transport design

The initial local diagnostic remapped UDP 30000 onto two distinct loopback ports.
That established a retail LAN baseline before selecting WebRTC. The shipping
bridge in `port/haze_coop.cpp` keeps real loopback sockets underneath the SDK so
its select/poll behavior remains intact, and carries unchanged datagrams over
unreliable, unordered data channels. Room peers map to `10.46.0.ID:30000`.
The [versioned signaling contract](../services/rendezvous/README.md) is available
for the separate browser integration.

## Observations

- Two unmodified local instances both bound UDP 30000. The client broadcast
  15-byte requests every approximately 0.51 seconds. No replies were observed.
  This alone does not distinguish Windows local broadcast behavior from a
  game problem.
- With separate loopback endpoints, the host received those requests and
  returned 91-byte replies. The client displayed the host, Selva Region,
  co-op mode, normal difficulty, and three available public slots.
- The host's configured `Host_46` name appeared in the other instance's game
  list. This verifies the profile display field in a real LAN advertisement.
- Discovery also exchanges an 11-byte packet and a 16-byte reply. Some outgoing
  addresses are on the guest stack, above `0xc0000000`; restricting the probe
  to the guest heap silently misses those packets.
- Entering discovery before a listener exists exposed a recovery problem in
  the initial loopback experiment. Leaving and returning created a second
  socket while the first descriptor remained allocated. Fresh-client discovery
  worked. Error handling and socket ownership still require investigation.
- With complete address remapping, the client loaded into the host's campaign
  and movement appeared in the other instance's view.
- The original teammate label used a stale character `NAME` property. The
  guarded `0062A280` hook matches the viewed character's entity handle (`+44`)
  to a network player's entity handle (`+160`), then copies its display name
  (`+16a`) into the existing 64-byte label buffer. The nearby voice lookup
  identifies the local player and cannot supply the teammate's name. Matching
  the full entity handle preserves the NPC fallback and avoids stale slots;
  profile keys, character properties and guest registers remain unchanged.
  Two local instances verified `Host_46` on the client and `Client_46` on the
  host, alongside synchronized movement. The regression exercises different
  local/remote slots, stale handles, missing names, bounds and register state.
- Closing the client caused the host to display the retail lost-server dialog.
  The Windows SDK socket path now disables UDP Port Unreachable
  reporting with `SIO_UDP_CONNRESET`. An end-to-end SDK regression sends to a
  closed loopback port repeatedly, then receives successfully from another peer.
  See [Microsoft's IOCTL reference](https://learn.microsoft.com/en-us/windows/win32/winsock/winsock-ioctls#sio_udp_connreset-opcode-setting-i-t3).
- With that correction, discovery started before the host recovered when the
  host became available. The host survived client closure, and a fresh client
  rejoined the ongoing campaign. Both copied profile files were unchanged.
- The independent WebRTC test connected four peers directly, exchanged binary
  datagrams, removed/rejoined a peer, and ended the room on host departure. A
  two-peer TURN test verified a selected relay route. The game bridge and native
  invite pages then passed campaign join, movement, client departure, rejoin
  with the same invite, and host-closure reproduction.
- With `PS3_NET_ONLINE=0` and no co-op service, a fresh instance loaded the
  single-player checkpoint.
- The user deferred the Windows VM test; cross-machine and separate-router
  validation will be follow-up work rather than a prerequisite for this PR.

## Remaining acceptance work

Validate campaign progression beyond join and movement and four-player gameplay. Competitive modes
remain a separate inventory. Local loopback evidence cannot establish Internet
reachability; the separate-machine and router tests are deferred.
