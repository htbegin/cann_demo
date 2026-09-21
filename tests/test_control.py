"""CPU-only tests for the actual C control code. No NPU/RDMA emulation."""
import json
import socket
import subprocess
import sys
import time
import unittest

PROBE = sys.argv.pop(1)
FRAME = 256


def frame(text):
    return text.encode().ljust(FRAME, b"\0")


def receive(sock):
    result = bytearray()
    while len(result) < FRAME:
        part = sock.recv(FRAME - len(result))
        if not part:
            raise EOFError("short frame")
        result.extend(part)
    return bytes(result).rstrip(b"\0")


def metadata(kind=0, size=1048576, address=0x123456780000, device=1,
             rank=0, host="10.0.0.1", npu="192.168.0.1"):
    return f"A2ROCE1 {kind} {size} {address} {device} {rank} {host} {npu}"


class ControlTests(unittest.TestCase):
    def probe(self, *args):
        return subprocess.run([PROBE, *args], capture_output=True, text=True, timeout=3)

    def test_metadata_roundtrip(self):
        message = metadata(kind=1)
        result = self.probe("decode", message)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), message)

    def test_bad_metadata(self):
        cases = [metadata().replace("A2ROCE1", "A2ROCE2"), metadata(kind=2),
                 metadata(size=0), metadata(size=-1), metadata(address=0),
                 metadata(address=2**64 - 1), metadata(device=-1), metadata(rank=2),
                 metadata(host="0.0.0.0"), metadata(npu="bad"), metadata() + " extra"]
        for message in cases:
            with self.subTest(message=message):
                self.assertNotEqual(self.probe("decode", message).returncode, 0)

    def test_same_host_rank_table(self):
        result = self.probe("table", metadata(), metadata(device=3, rank=1, npu="192.168.0.3"))
        self.assertEqual(result.returncode, 0, result.stderr)
        table = json.loads(result.stdout)
        self.assertEqual(table["server_count"], "1")
        devices = table["server_list"][0]["device"]
        self.assertEqual([d["rank_id"] for d in devices], ["0", "1"])
        self.assertEqual([d["device_id"] for d in devices], ["1", "3"])

    def test_two_host_rank_table(self):
        result = self.probe("table", metadata(kind=1),
                            metadata(kind=1, rank=1, host="10.0.0.2", npu="192.168.0.9"))
        self.assertEqual(result.returncode, 0, result.stderr)
        table = json.loads(result.stdout)
        self.assertEqual(table["server_count"], "2")
        self.assertEqual([s["server_id"] for s in table["server_list"]], ["10.0.0.1", "10.0.0.2"])

    def test_peer_mismatch(self):
        for peer in [metadata(kind=1, rank=1), metadata(size=8, rank=1), metadata(),
                     metadata(rank=1), metadata(rank=1, npu="192.168.0.2")]:
            with self.subTest(peer=peer):
                self.assertNotEqual(self.probe("table", metadata(), peer).returncode, 0)

    def run_client(self, action, success):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(2)
            with subprocess.Popen([PROBE, "client", str(listener.getsockname()[1])],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
                with listener.accept()[0] as peer:
                    peer.settimeout(2)
                    action(peer)
                _, error = process.communicate(timeout=2)
                self.assertEqual(process.returncode == 0, success, error)

    def test_fragmented_roundtrip(self):
        def action(peer):
            for byte in frame("HELLO"):
                peer.sendall(bytes([byte]))
            self.assertEqual(receive(peer), b"READY")
            peer.sendall(frame("PASS"))
            self.assertEqual(receive(peer), b"CLOSED")
        self.run_client(action, True)

    def test_eof_mid_frame(self):
        self.run_client(lambda peer: peer.sendall(b"HEL"), False)

    def test_malformed_frame(self):
        self.run_client(lambda peer: peer.sendall(frame("HELLO")[:-1] + b"x"), False)

    def test_stalled_peer_timeout(self):
        self.run_client(lambda peer: time.sleep(0.7), False)

    def test_server_accept(self):
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        with subprocess.Popen([PROBE, "server", str(port)], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE) as process:
            deadline = time.monotonic() + 0.4
            while True:
                try:
                    peer = socket.create_connection(("127.0.0.1", port), timeout=1)
                    break
                except ConnectionRefusedError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.005)
            with peer:
                peer.sendall(frame("HELLO"))
                self.assertEqual(receive(peer), b"READY")
                peer.sendall(frame("PASS"))
                self.assertEqual(receive(peer), b"CLOSED")
            _, error = process.communicate(timeout=2)
            self.assertEqual(process.returncode, 0, error)


if __name__ == "__main__":
    unittest.main()
