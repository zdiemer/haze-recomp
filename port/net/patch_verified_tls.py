"""Keep certificate verification enabled with an explicit Windows CA bundle.

libdatachannel v0.24.6 unconditionally disables WebSocket TLS verification on
Windows. Haze supplies Windows trust roots (or a configured private CA), so its
MbedTLS backend can use VerifiedTlsTransport there as it does on other hosts.
Generate a build-local source copy; never modify the fetched upstream tree.
"""
import hashlib
from pathlib import Path
import sys

DIGEST = 'ab7332168c7e74bfe5c1c36627a833f82cbf29dd5d8d9c7a2565fcd64b51d776'
OLD = '''#ifdef _WIN32
		if (std::exchange(verify, false)) {
			PLOG_WARNING << "TLS certificate verification with root CA is not supported on Windows";
		}
#endif'''
NEW = '''#ifdef _WIN32
		// Haze supplies the trusted CA bundle to the MbedTLS verifier.
		if (verify && !config.caCertificatePemFile)
			throw std::runtime_error("A trusted CA bundle is required on Windows");
#endif'''


def patch(source):
    if hashlib.sha256(source.encode()).hexdigest() != DIGEST or source.count(OLD) != 1:
        raise ValueError('libdatachannel WebSocket TLS changed; audit certificate verification')
    return source.replace(OLD, NEW)


if __name__ == '__main__':
    output = Path(sys.argv[2])
    text = patch(Path(sys.argv[1]).read_text())
    if not output.exists() or output.read_text() != text:
        output.write_text(text)
