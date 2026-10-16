"""Run native direct and forced-TURN tests using loopback-only services."""
import argparse
import asyncio
from pathlib import Path
from websockets.asyncio.server import serve
from server import Rendezvous


async def main(build):
    suffix = '.exe' if (build / 'test_network_session.exe').exists() else ''
    relay = await asyncio.create_subprocess_exec(str(build / ('network_relay' + suffix)),
                                               stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE)
    try:
        line = await asyncio.wait_for(relay.stdout.readline(), 5)
        if b'TURN fixture ready 3479' not in line:
            raise RuntimeError('TURN fixture did not start')
        for force_relay in (False, True):
            service = Rendezvous()
            if force_relay:
                service.ice = lambda: [{'urls': 'turn:127.0.0.1:3479', 'username': 'haze-test', 'credential': 'local-test-only'}]
            async with serve(service.handler, '127.0.0.1', 0, max_size=20000) as server:
                port = server.sockets[0].getsockname()[1]
                args = [str(build / ('test_network_session' + suffix)), f'ws://127.0.0.1:{port}/v1']
                if force_relay:
                    args.append('--relay')
                process = await asyncio.create_subprocess_exec(*args)
                result = await asyncio.wait_for(process.wait(), 120)
                if result:
                    raise RuntimeError(f'{"Relay" if force_relay else "Direct"} test exited {result}')
                print('Forced TURN passed' if force_relay else 'Direct connection passed', flush=True)
    finally:
        relay.stdin.write(b'quit\n')
        await relay.stdin.drain()
        await asyncio.wait_for(relay.wait(), 10)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    asyncio.run(main(parser.parse_args().build.resolve()))
