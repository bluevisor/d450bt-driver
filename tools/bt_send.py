#!/usr/bin/env python3
"""Robust sender for /dev/cu.D450BT-Z.

Opens the port non-blocking (a plain open() blocks forever while macOS
re-establishes the Bluetooth SPP link), then polls until the link is
writable and streams the payload in small chunks.
"""
import fcntl
import os
import select
import sys
import time

PORT = "/dev/cu.D450BT-Z"
CHUNK = 256
CHUNK_DELAY = 0.03
CONNECT_TIMEOUT = 25


def send(payload: bytes, port: str = PORT) -> None:
    fd = os.open(port, os.O_WRONLY | os.O_NONBLOCK | os.O_NOCTTY)
    try:
        # Keep O_NONBLOCK: on an un-established BT link, select() can
        # report writable while a blocking write would hang forever on
        # carrier. Non-blocking writes fail fast with EAGAIN instead.
        deadline = time.time() + CONNECT_TIMEOUT
        sent = 0
        while sent < len(payload):
            _, writable, _ = select.select([], [fd], [], 1.0)
            if not writable:
                if time.time() > deadline:
                    raise TimeoutError(
                        f"Port never became writable within {CONNECT_TIMEOUT}s "
                        "- is the printer awake?")
                continue
            try:
                n = os.write(fd, payload[sent:sent + CHUNK])
            except BlockingIOError:
                if time.time() > deadline:
                    raise TimeoutError(
                        f"Link not accepting data within {CONNECT_TIMEOUT}s "
                        "- is the printer awake?") from None
                time.sleep(0.2)
                continue
            except OSError as e:
                raise OSError(f"write failed after {sent} bytes: {e}") from e
            sent += n
            deadline = time.time() + CONNECT_TIMEOUT
            time.sleep(CHUNK_DELAY)
            if sent % (CHUNK * 40) < CHUNK:
                print(f"  {sent}/{len(payload)} bytes", flush=True)
        # Give the OS buffer time to drain before closing the link.
        time.sleep(1.0)
    finally:
        os.close(fd)


if __name__ == "__main__":
    data = sys.stdin.buffer.read() if len(sys.argv) < 2 else open(sys.argv[1], "rb").read()
    print(f"Sending {len(data)} bytes to {PORT} ...", flush=True)
    send(data)
    print("Done.", flush=True)
