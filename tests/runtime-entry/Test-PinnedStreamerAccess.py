#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the packaged streamer's pinned HTTPS access and control on loopback."""

import argparse
import base64
from contextlib import ExitStack
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
from threading import Thread
from urllib.parse import urlsplit


HOST_ID = "a2519288-e7d1-44b0-b172-79128ba6229a"
OTHER_ID = "6919e7e7-2428-44b8-a06d-a244c4877850"
AUTHORIZED = b"aurora-route-access/1,authorized"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def make_certificate(openssl, directory, name):
    cert = directory / f"{name}.crt"
    key = directory / f"{name}.key"
    config = directory / "openssl.cnf"
    config.write_text("[req]\ndistinguished_name=dn\n[dn]\n", encoding="ascii")
    subprocess.run(
        [openssl, "genrsa", "-traditional", "-out", str(key), "2048"],
        check=True, capture_output=True, creationflags=NO_WINDOW,
    )
    subprocess.run(
        [openssl, "req", "-new", "-x509", "-config", str(config), "-key", str(key), "-out", str(cert),
         "-days", "1", "-subj", f"/CN={name}"],
        check=True, capture_output=True, creationflags=NO_WINDOW,
    )
    return cert, key


def store_client_identity(directory, certificate, key):
    def pem_value(path):
        return "@ByteArray(" + path.read_text(encoding="ascii").replace("\n", r"\n") + ")"

    config = directory / "Moonlight Game Streaming Project" / "Moonlight.ini"
    config.parent.mkdir()
    config.write_text(
        f"[General]\ncertificate={pem_value(certificate)}\nkey={pem_value(key)}\n",
        encoding="ascii",
    )
    (directory / "portable.dat").touch()


class GameStreamFixture(BaseHTTPRequestHandler):
    def peer_certificate(self):
        if isinstance(self.connection, ssl.SSLSocket):
            return self.connection.getpeercert(binary_form=True)
        return None

    def respond(self, status, content_type, payload):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        self.server.received.append((self.path.partition("?")[0], self.peer_certificate()))
        if self.server.client_allowed:
            xml = (f'<root status_code="200"><uniqueid>{self.server.host_id}</uniqueid>'
                   "<PairStatus>1</PairStatus></root>")
        else:
            xml = '<root status_code="401" status_message="Client not authorized" />'
        self.respond(200, "application/xml", xml.encode("ascii"))

    def do_POST(self):
        url = urlsplit(self.path)
        self.server.posts.append((url.path, url.query, self.peer_certificate()))
        if url.path == "/aurora-session-privacy" and self.server.client_allowed:
            if not self.server.privacy_available:
                self.respond(404, "text/html", b"<html><body>Not found</body></html>")
            elif url.query == "action=status":
                self.respond(200, "text/plain", self.server.privacy_state)
            else:
                self.respond(400, "text/plain", b"unsupported fixture action")
        elif url.path == "/aurora-video-bitrate-v2" and self.server.client_allowed and url.query == "action=status":
            self.respond(200, "text/plain", b"status " + b"a" * 32 + b" 20000 16000 25000 7")
        else:
            self.respond(401, "text/plain", b"unauthorized")

    def log_message(self, *_):
        pass


class LoopbackServer(HTTPServer):
    allow_reuse_address = False


class LoopbackIpv6Server(LoopbackServer):
    address_family = socket.AF_INET6


def bind_gamestream_ports(stack, address):
    server_type = LoopbackIpv6Server if address == "::1" else LoopbackServer
    for _ in range(16):
        with ExitStack() as attempt:
            base = attempt.enter_context(server_type((address, 0), GameStreamFixture))
            https_port = base.server_port - 5
            try:
                tls = attempt.enter_context(server_type((address, https_port), GameStreamFixture))
            except OSError:
                continue
            stack.enter_context(attempt.pop_all())
            return base, tls
    raise RuntimeError("Could not reserve temporary GameStream ports on loopback.")


def run_server(stack, server):
    server.received = []
    server.posts = []
    server.host_id = HOST_ID
    server.client_allowed = True
    server.privacy_available = True
    server.privacy_state = b"off"
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    stack.callback(thread.join)
    stack.callback(server.shutdown)


def run_access(streamer, directory, endpoint, fingerprint, expected_id=HOST_ID, action="access", command="route-probe"):
    environment = dict(os.environ)
    for name in (
        "AURORA_GAMESTREAM_HOST_UUID",
        "AURORA_GAMESTREAM_RELAY_BASE",
        "AURORA_GAMESTREAM_PAIRED_HOST_UUID",
    ):
        environment.pop(name, None)
    environment["AURORA_GAMESTREAM_HOST_CERTIFICATE_SHA256"] = fingerprint
    return subprocess.run(
        [str(streamer), "--aurora-runtime", command, endpoint, expected_id, action],
        cwd=directory, env=environment, timeout=12, capture_output=True, creationflags=NO_WINDOW,
    )


def check_client_proof(streamer, openssl, directory, host_der, client_der, client_certificate):
    host_fingerprint = hashlib.sha256(host_der).digest()
    # Reproducible, synthetic nonce and Tailscale scope.
    challenge = b"n" * 32 + host_fingerprint + b"s" * 32
    env = dict(os.environ)
    env["AURORA_GAMESTREAM_HOST_CERTIFICATE_SHA256"] = host_fingerprint.hex()
    env.pop("AURORA_GAMESTREAM_RELAY_BASE", None)
    env.pop("AURORA_GAMESTREAM_HOST_UUID", None)

    def collect(encoded_challenge, environment):
        return subprocess.run(
            [str(streamer), "--aurora-runtime", "client-proof", encoded_challenge],
            cwd=directory, env=environment, timeout=12, capture_output=True, creationflags=NO_WINDOW,
        )

    response = collect(challenge.hex(), env)
    assert response.returncode == 0, response.stderr
    version, encoded_certificate, encoded_signature = response.stdout.strip().split(b",")
    assert version == b"aurora-client-proof/1"
    assert base64.b64decode(encoded_certificate, validate=True) == client_der
    signature = directory / "signature.bin"
    signature.write_bytes(base64.b64decode(encoded_signature, validate=True))
    public_key = directory / "client.pub"
    subprocess.run(
        [openssl, "x509", "-in", str(client_certificate), "-pubkey", "-noout", "-out", str(public_key)],
        check=True, capture_output=True, creationflags=NO_WINDOW,
    )

    def verify(transcript_challenge):
        transcript = directory / "transcript.bin"
        transcript.write_bytes(
            b"aurora-client-proof/1\0" + transcript_challenge + hashlib.sha256(client_der).digest()
        )
        return subprocess.run(
            [openssl, "dgst", "-sha256", "-verify", str(public_key), "-signature", str(signature), str(transcript)],
            capture_output=True, creationflags=NO_WINDOW,
        ).returncode

    assert verify(challenge) == 0
    assert verify(b"x" + challenge[1:]) != 0
    assert verify(challenge[:32] + b"x" * 32 + challenge[64:]) != 0
    assert verify(challenge[:-1] + b"x") != 0
    for rejected in (
        collect(challenge.hex()[:-1], env),
        collect(challenge.hex(), {**env, "AURORA_GAMESTREAM_HOST_CERTIFICATE_SHA256": "0" * 64}),
        collect((bytes(32) + challenge[32:]).hex(), env),
    ):
        assert rejected.returncode != 0 and not rejected.stdout


def check_session_controls(run, secure, host_fingerprint, client_der):
    replies = (
        ("privacy", "/aurora-session-privacy", b"off"),
        ("bitrate", "/aurora-video-bitrate-v2", b"status " + b"a" * 32 + b" 20000 16000 25000 7"),
    )
    for command, path, payload in replies:
        secure.received.clear()
        secure.posts.clear()

        confirmed = run(host_fingerprint, command=command, action="status")
        assert confirmed.returncode == 0 and confirmed.stdout.strip() == payload, confirmed
        assert secure.received == [("/serverinfo", client_der)]
        assert secure.posts == [(path, "action=status", client_der)]
        secure.received.clear()
        secure.posts.clear()

        for invalid_pin in ("0" * 64, ""):
            refused = run(invalid_pin, command=command, action="status")
            assert refused.returncode != 0 and not refused.stdout.strip(), refused
        assert not secure.received and not secure.posts, "A control command must verify the pin before HTTP."

        wrong_host = run(host_fingerprint, OTHER_ID, command=command, action="status")
        assert wrong_host.returncode != 0 and not wrong_host.stdout.strip() and not secure.posts, wrong_host
        secure.client_allowed = False
        denied = run(host_fingerprint, command=command, action="status")
        assert denied.returncode != 0 and not denied.stdout.strip() and not secure.posts, denied
        secure.client_allowed = True

    secure.privacy_state = b"unavailable"
    unconfirmed = run(host_fingerprint, command="privacy", action="status")
    assert unconfirmed.returncode == 0 and unconfirmed.stdout.strip() == b"unavailable", unconfirmed
    secure.privacy_available = False
    missing = run(host_fingerprint, command="privacy", action="status")
    assert missing.returncode == 0 and missing.stdout.strip() == b"unsupported", missing
    rejected = run(host_fingerprint, command="privacy", action="on")
    assert rejected.returncode != 0 and not rejected.stdout.strip(), rejected
    assert secure.posts == [("/aurora-session-privacy", query, client_der)
                            for query in ("action=status", "action=status", "action=on")], secure.posts


def check_access_and_route_probe(streamer, directory, host_fingerprint, client_der, context, address, check_controls):
    with ExitStack() as stack:
        plain, secure = bind_gamestream_ports(stack, address)
        secure.socket = context.wrap_socket(secure.socket, server_side=True)
        run_server(stack, secure)
        run_server(stack, plain)
        endpoint = f"[{address}]:{plain.server_port}" if address == "::1" else f"{address}:{plain.server_port}"

        trusted = run_access(streamer, directory, endpoint, host_fingerprint)
        assert trusted.returncode == 0 and trusted.stdout.strip() == AUTHORIZED, trusted
        assert secure.received == [("/serverinfo", client_der)], secure.received

        incorrect_pin = run_access(streamer, directory, endpoint, "0" * 64)
        assert incorrect_pin.returncode != 0 and not incorrect_pin.stdout.strip(), incorrect_pin
        missing_pin = run_access(streamer, directory, endpoint, "")
        assert missing_pin.returncode != 0 and not missing_pin.stdout.strip(), missing_pin
        assert len(secure.received) == 1, "The incorrect pin must stop before sending an HTTPS request."

        other_host = run_access(streamer, directory, endpoint, host_fingerprint, OTHER_ID)
        assert other_host.returncode != 0 and not other_host.stdout.strip(), other_host
        assert len(secure.received) == 2

        secure.client_allowed = False
        denied = run_access(streamer, directory, endpoint, host_fingerprint)
        assert denied.returncode != 0 and not denied.stdout.strip(), denied
        assert len(secure.received) == 3

        secure.client_allowed = True
        quality = run_access(streamer, directory, endpoint, host_fingerprint, action="sample")
        assert quality.returncode == 0 and quality.stdout.startswith(b"aurora-route/1,3,"), quality
        assert secure.received == [("/serverinfo", client_der)] * 6, secure.received
        print(f"{address}: pinned HTTPS, rejected pin/UUID/client, and three route samples passed.")
        if check_controls:
            def invoke(pin, expected_id=HOST_ID, **operation):
                return run_access(streamer, directory, endpoint, pin, expected_id, **operation)

            check_session_controls(invoke, secure, host_fingerprint, client_der)
            print(f"{address}: pinned privacy and bitrate controls passed without a cached host certificate.")
        assert not plain.received and not plain.posts, "Aurora must never fall back to the plaintext port."


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("streamer", type=Path)
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--check-session-controls", action="store_true")
    args = parser.parse_args()
    streamer = args.streamer.resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="aurora-stream-access-") as temp:
        directory = Path(temp)
        host_certificate, host_key = make_certificate(args.openssl, directory, "localhost")
        client_certificate, client_key = make_certificate(args.openssl, directory, "test-client")
        store_client_identity(directory, client_certificate, client_key)
        host_der = ssl.PEM_cert_to_DER_cert(host_certificate.read_text(encoding="ascii"))
        client_der = ssl.PEM_cert_to_DER_cert(client_certificate.read_text(encoding="ascii"))
        host_fingerprint = hashlib.sha256(host_der).hexdigest()
        check_client_proof(streamer, args.openssl, directory, host_der, client_der, client_certificate)

        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(host_certificate, host_key)
        context.load_verify_locations(cafile=str(client_certificate))
        context.verify_mode = ssl.CERT_REQUIRED
        check_access_and_route_probe(streamer, directory, host_fingerprint, client_der, context, "127.0.0.1",
                                     args.check_session_controls)
        if socket.has_ipv6:
            check_access_and_route_probe(streamer, directory, host_fingerprint, client_der, context, "::1",
                                         args.check_session_controls)
        else:
            print("IPv6 route probe skipped because the test system has no IPv6 support.")


if __name__ == "__main__":
    main()
