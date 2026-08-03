#!/usr/bin/env python3
"""
Minimal TCP echo server for HelixOS M15 smoke test.
Listens on 0.0.0.0:8080, echoes back each received line.
Started by Makefile smoke-net, killed after QEMU exits.
"""
import socket, sys, threading, signal

PORT = 8080
running = True

def handle(conn, addr):
    try:
        while running:
            data = conn.recv(1024)
            if not data:
                break
            msg = data.decode('ascii', errors='replace')
            reply = "ECHO:" + msg
            conn.sendall(reply.encode('ascii'))
            sys.stderr.write(f"[tcp-echo] {addr} <- {msg.strip()!r}\n")
    except Exception:
        pass
    finally:
        try: conn.close()
        except: pass

def main():
    global running
    signal.signal(signal.SIGTERM, lambda *_: setattr(sys.modules[__name__], 'running', False))
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', PORT))
    srv.listen(4)
    srv.settimeout(1.0)
    sys.stderr.write(f"[tcp-echo] listening on port {PORT}\n")
    try:
        while running:
            try:
                conn, addr = srv.accept()
                t = threading.Thread(target=handle, args=(conn, addr), daemon=True)
                t.start()
            except socket.timeout:
                continue
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()

if __name__ == '__main__':
    main()
