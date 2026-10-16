"""Invite-only Haze signaling. Gameplay travels over WebRTC, never this socket."""
import asyncio
import base64
from collections import defaultdict, deque
from dataclasses import dataclass, field
import hashlib
import hmac
import json
import os
import secrets
import time

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

GAME = 'haze-blus30094-v1'
ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'


@dataclass
class Room:
    code: str
    members: dict = field(default_factory=dict)


class Rendezvous:
    def __init__(self, *, turn_host='', turn_secret='', max_rooms=256):
        self.rooms = {}
        self.turn_host = turn_host
        self.turn_secret = turn_secret
        self.max_rooms = max_rooms
        self.connections = defaultdict(int)
        self.attempts = {}

    def ice(self):
        if not self.turn_host:
            return []  # Explicit local test mode only.
        username = f'{int(time.time()) + 86400}:{secrets.token_hex(8)}'
        password = base64.b64encode(hmac.new(self.turn_secret.encode(), username.encode(), hashlib.sha1).digest()).decode()
        return [{'urls': f'stun:{self.turn_host}:3478'},
                {'urls': f'turn:{self.turn_host}:3478', 'username': username, 'credential': password}]

    async def send(self, ws, message):
        await ws.send(json.dumps(message, separators=(',', ':')))

    async def error(self, ws, code):
        await self.send(ws, {'op': 'error', 'code': code})

    def allow_attempt(self, ip):
        now = time.monotonic()
        # Bounded, time-expiring rate state even when requests use many IPs.
        for key in list(self.attempts):
            if not self.attempts[key] or self.attempts[key][-1] < now - 60:
                del self.attempts[key]
        bucket = self.attempts.setdefault(ip, deque())
        while bucket and bucket[0] < now - 60:
            bucket.popleft()
        if len(bucket) >= 30 or len(self.attempts) > 4096:
            return False
        bucket.append(now)
        return True

    async def handler(self, ws):
        if ws.request.path != '/v1':
            await ws.close(1008, 'Use /v1')
            return
        ip = ws.remote_address[0] if ws.remote_address else 'local'
        if self.connections[ip] >= 16:
            await ws.close(1008, 'Too many connections')
            return
        self.connections[ip] += 1
        room = None
        member = None
        try:
            request = json.loads(await asyncio.wait_for(ws.recv(), 10))
            if not isinstance(request, dict) or type(request.get('v')) is not int or request.get('v') != 1 or request.get('game') != GAME:
                await self.error(ws, 'version')
                return
            if not self.allow_attempt(ip):
                await self.error(ws, 'rate_limit')
                return
            if request.get('op') == 'create':
                if len(self.rooms) >= self.max_rooms:
                    await self.error(ws, 'busy')
                    return
                while True:
                    code = ''.join(secrets.choice(ALPHABET) for _ in range(12))
                    if code not in self.rooms:
                        break
                room = Room(code)
                self.rooms[code] = room
                member = 1
            elif request.get('op') == 'join':
                code = request.get('code', '')
                if not isinstance(code, str) or len(code) != 12 or any(c not in ALPHABET for c in code):
                    await self.error(ws, 'invalid_code')
                    return
                room = self.rooms.get(code)
                if room is None:
                    await self.error(ws, 'not_found')
                    return
                if len(room.members) >= 4:
                    room = None
                    await self.error(ws, 'full')
                    return
                member = next(i for i in range(2, 5) if i not in room.members)
            else:
                await self.error(ws, 'protocol')
                return
            peers = list(room.members.items())
            room.members[member] = ws
            await self.send(ws, {'op': 'joined', 'v': 1, 'id': member, 'code': room.code,
                                 'peers': [i for i, _ in peers], 'ice': self.ice()})
            for _, peer in peers:
                try:
                    await self.send(peer, {'op': 'peer', 'id': member})
                except ConnectionClosed:
                    pass
            recent = deque()
            async for wire in ws:
                now = time.monotonic()
                while recent and recent[0] < now - 10:
                    recent.popleft()
                if len(recent) >= 200:
                    await ws.close(1008, 'Signaling rate exceeded')
                    break
                recent.append(now)
                message = json.loads(wire)
                if not isinstance(message, dict) or message.get('op') != 'signal':
                    await self.error(ws, 'protocol')
                    break
                target = message.get('to')
                if type(target) is not int or target == member or target not in room.members:
                    await self.error(ws, 'peer_gone')
                    continue
                description, candidate = message.get('description'), message.get('candidate')
                forwarded = {'op': 'signal', 'from': member}
                if isinstance(description, dict) and candidate is None:
                    if description.get('type') not in ('offer', 'answer') or not isinstance(description.get('sdp'), str) or len(description['sdp']) > 16384:
                        raise ValueError('Invalid description')
                    # The lower ID is always the offerer; avoid negotiation glare.
                    if (description['type'] == 'offer') != (member < target):
                        raise ValueError('Invalid offerer')
                    forwarded['description'] = {'type': description['type'], 'sdp': description['sdp']}
                elif isinstance(candidate, dict) and description is None:
                    if not isinstance(candidate.get('candidate'), str) or len(candidate['candidate']) > 1024 or not isinstance(candidate.get('mid'), str) or len(candidate['mid']) > 32:
                        raise ValueError('Invalid candidate')
                    forwarded['candidate'] = {'candidate': candidate['candidate'], 'mid': candidate['mid']}
                else:
                    raise ValueError('Invalid signal')
                try:
                    await self.send(room.members[target], forwarded)
                except ConnectionClosed:
                    await self.error(ws, 'peer_gone')
        except (ConnectionClosed, asyncio.TimeoutError):
            pass
        except (ValueError, TypeError, KeyError):
            try:
                await self.error(ws, 'protocol')
            except ConnectionClosed:
                pass
        finally:
            if room is not None and member in room.members and room.members[member] is ws:
                del room.members[member]
                if member == 1:
                    self.rooms.pop(room.code, None)
                    remaining = list(room.members.values())
                    room.members.clear()
                    for peer in remaining:
                        try:
                            await self.send(peer, {'op': 'closed'})
                            await peer.close(1000, 'Host left')
                        except ConnectionClosed:
                            pass
                else:
                    for peer in list(room.members.values()):
                        try:
                            await self.send(peer, {'op': 'left', 'id': member})
                        except ConnectionClosed:
                            pass
            self.connections[ip] -= 1
            if not self.connections[ip]:
                del self.connections[ip]


async def main():
    host = os.getenv('HAZE_SIGNAL_BIND', '127.0.0.1')
    port = int(os.getenv('HAZE_SIGNAL_PORT', '8765'))
    turn_host, turn_secret = os.getenv('HAZE_TURN_HOST', ''), os.getenv('HAZE_TURN_SECRET', '')
    if not (turn_host and turn_secret) and os.getenv('HAZE_LOCAL_TEST') != '1':
        raise SystemExit('Set HAZE_TURN_HOST and HAZE_TURN_SECRET, or HAZE_LOCAL_TEST=1 for local tests.')
    service = Rendezvous(turn_host=turn_host, turn_secret=turn_secret)
    # The TLS reverse proxy should connect from loopback. Forwarded headers are
    # deliberately not trusted as client identities; configure per-IP limits at
    # that proxy too. Protocol limits apply here regardless of proxy settings.
    async with serve(service.handler, host, port, max_size=20000, max_queue=16,
                     ping_interval=20, ping_timeout=20, close_timeout=2,
                     compression=None):
        print(f'Haze signaling listening on {host}:{port}', flush=True)
        await asyncio.Future()


if __name__ == '__main__':
    asyncio.run(main())
