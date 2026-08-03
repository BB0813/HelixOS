#!/usr/bin/env python3
"""
Host-side TCP client for HelixOS M16 passive smoke test.
Connects to guest TCP server (via QEMU hostfwd port 8081),
sends a test message, verifies echo reply.
Exits 0 on success, 1 on failure.
"""
import socket, sys, time

HOST = "127.0.0.1"
PORT = 8081
PAYLOAD = b"HELIX_TCP_PASSIVE"
EXPECTED = b"ECHO:HELIX_TCP_PASSIVE"
TIMEOUT = 30

def main():
    deadline = time.time() + TIMEOUT
    sock = None
    # Retry connection until guest server is ready
    while time.time() < deadline:
        try:
            sock = socket.create_connection((HOST, PORT), timeout=2)
            break
        except (ConnectionRefusedError, OSError):
            time.sleep(0.5)

    if sock is None:
        sys.stderr.write("[tcp-host-client] connection timed out\n")
        sys.exit(1)

    try:
        sock.sendall(PAYLOAD)
        sys.stderr.write(f"[tcp-host-client] sent {PAYLOAD!r}\n")
        sock.settimeout(5.0)
        data = b""
        while len(data) < len(EXPECTED):
            chunk = sock.recv(1024)
            if not chunk:
                break
            data += chunk
        sys.stderr.write(f"[tcp-host-client] received {data!r}\n")
        if data == EXPECTED:
            print("HelixTcpPassiveOK")
            sys.exit(0)
        else:
            sys.stderr.write(f"[tcp-host-client] mismatch: expected {EXPECTED!r}\n")
            sys.exit(1)
    finally:
        sock.close()

if __name__ == "__main__":
    main()
