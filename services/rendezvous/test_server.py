import asyncio
import json
import unittest
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed
from server import GAME, Rendezvous


class ServerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.service = Rendezvous()
        self.server = await serve(self.service.handler, '127.0.0.1', 0, max_size=20000)
        self.url = f'ws://127.0.0.1:{self.server.sockets[0].getsockname()[1]}/v1'
        self.clients = []

    async def asyncTearDown(self):
        for ws in self.clients:
            await ws.close()
        self.server.close()
        await self.server.wait_closed()
        self.assertFalse(self.service.rooms)
        self.assertFalse(self.service.connections)

    async def request(self, **request):
        ws = await connect(self.url)
        self.clients.append(ws)
        await ws.send(json.dumps({'v': 1, 'game': GAME, **request}))
        return ws, await self.recv(ws)

    async def recv(self, ws):
        return json.loads(await asyncio.wait_for(ws.recv(), 2))

    async def test_create_join_signal_isolation_and_host_close(self):
        host, room = await self.request(op='create')
        outsider, other = await self.request(op='create')
        self.assertNotEqual(room['code'], other['code'])
        client, joined = await self.request(op='join', code=room['code'])
        self.assertEqual(joined['peers'], [1])
        self.assertEqual(await self.recv(host), {'op': 'peer', 'id': 2})
        await host.send(json.dumps({'op': 'signal', 'to': 2, 'from': 99, 'description': {'type': 'offer', 'sdp': 'test'}}))
        self.assertEqual(await self.recv(client), {'op': 'signal', 'from': 1, 'description': {'type': 'offer', 'sdp': 'test'}})
        await outsider.send(json.dumps({'op': 'signal', 'to': 2, 'candidate': {'candidate': 'x', 'mid': '0'}}))
        self.assertEqual((await self.recv(outsider))['code'], 'peer_gone')
        await host.close()
        self.assertEqual(await self.recv(client), {'op': 'closed'})
        _, error = await self.request(op='join', code=room['code'])
        self.assertEqual(error['code'], 'not_found')

    async def test_capacity_departure_and_rejoin(self):
        host, room = await self.request(op='create')
        peers = []
        for expected in range(2, 5):
            ws, joined = await self.request(op='join', code=room['code'])
            self.assertEqual(joined['id'], expected)
            for existing in [host, *peers]:
                self.assertEqual(await self.recv(existing), {'op': 'peer', 'id': expected})
            peers.append(ws)
        _, error = await self.request(op='join', code=room['code'])
        self.assertEqual(error['code'], 'full')
        await peers[0].close()
        for existing in [host, *peers[1:]]:
            self.assertEqual(await self.recv(existing), {'op': 'left', 'id': 2})
        _, joined = await self.request(op='join', code=room['code'])
        self.assertEqual(joined['id'], 2)

    async def test_invalid_version_code_and_message(self):
        _, error = await self.request(op='create', v=2)
        self.assertEqual(error['code'], 'version')
        _, error = await self.request(op='create', v=True)
        self.assertEqual(error['code'], 'version')
        _, error = await self.request(op='join', code='bad')
        self.assertEqual(error['code'], 'invalid_code')
        host, _ = await self.request(op='create')
        await host.send('[]')
        self.assertEqual((await self.recv(host))['code'], 'protocol')

    async def test_unknown_path_is_rejected(self):
        async with connect(self.url.replace('/v1', '/other')) as ws:
            with self.assertRaises(ConnectionClosed):
                await ws.recv()
        self.assertFalse(self.service.rooms)

    async def test_client_cannot_send_offer_to_host(self):
        host, room = await self.request(op='create')
        client, _ = await self.request(op='join', code=room['code'])
        await self.recv(host)
        await client.send(json.dumps({'op': 'signal', 'to': 1, 'description': {'type': 'offer', 'sdp': 'fake'}}))
        self.assertEqual((await self.recv(client))['code'], 'protocol')
        self.assertEqual(await self.recv(host), {'op': 'left', 'id': 2})

    async def test_request_rate_limit(self):
        for _ in range(30):
            self.assertTrue(self.service.allow_attempt('test'))
        self.assertFalse(self.service.allow_attempt('test'))
        self.assertTrue(self.service.allow_attempt('other'))


if __name__ == '__main__':
    unittest.main()
