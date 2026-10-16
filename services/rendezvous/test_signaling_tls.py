"""Exercise native WSS certificate and hostname validation with ephemeral keys."""
import argparse
import asyncio
from pathlib import Path
import ssl
import subprocess
import tempfile
from websockets.asyncio.server import serve
from server import Rendezvous


async def main(build, openssl):
    with tempfile.TemporaryDirectory(prefix='haze-tls-') as directory:
        directory = Path(directory)
        cert, key = directory / 'cert.pem', directory / 'key.pem'
        subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(key), '-out', str(cert), '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wrong_cert = directory / 'wrong.pem'
        subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(directory / 'wrong-key.pem'), '-out', str(wrong_cert), '-days', '1',
                        '-subj', '/CN=Unrelated test CA'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        service = Rendezvous()
        async with serve(service.handler, '127.0.0.1', 0, ssl=context, max_size=20000) as server:
            port = server.sockets[0].getsockname()[1]
            executable = build / ('test_network_session.exe' if (build / 'test_network_session.exe').exists() else 'test_network_session')
            cases = [
                [f'wss://localhost:{port}/v1', f'--ca={cert}'],
                [f'wss://localhost:{port}/v1', f'--ca={wrong_cert}', '--expect-failure'],
                [f'wss://localhost:{port}/v1', '--expect-failure'],
                [f'wss://127.0.0.1:{port}/v1', f'--ca={cert}', '--expect-failure'],
            ]
            for args in cases:
                process = await asyncio.create_subprocess_exec(str(executable), *args)
                result = await asyncio.wait_for(process.wait(), 120)
                if result:
                    raise RuntimeError(f'TLS test exited {result}')
        print('WSS trusted CA, untrusted CA, and hostname mismatch passed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--openssl', default='openssl')
    args = parser.parse_args()
    asyncio.run(main(args.build.resolve(), args.openssl))
