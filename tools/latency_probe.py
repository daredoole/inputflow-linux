#!/usr/bin/env python3
"""Cross-host ACK latency probe for InputFlow / Mouse Without Borders testing."""

from __future__ import annotations

import argparse
import math
import os
import socket
import struct
import sys
import time
from dataclasses import dataclass
from typing import Iterable


MAGIC = b"MWBLP001"
VERSION = 1
TYPE_PROBE = 1
TYPE_ACK = 2
TYPE_DONE = 3
PACKET = struct.Struct("!8sBBIQQQ")


@dataclass
class Sample:
    seq: int
    rtt_us: float
    estimated_one_way_us: float
    server_ack_us: float


@dataclass
class ResourceSnapshot:
    wall_ns: int
    cpu_ns: int
    rss_kib: int


def now_ns() -> int:
    return time.perf_counter_ns()


def cpu_ns() -> int:
    return time.process_time_ns()


def current_rss_kib() -> int:
    if sys.platform == "win32":
        try:
            import ctypes
            from ctypes import wintypes

            class ProcessMemoryCounters(ctypes.Structure):
                _fields_ = [
                    ("cb", wintypes.DWORD),
                    ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t),
                ]

            counters = ProcessMemoryCounters()
            counters.cb = ctypes.sizeof(ProcessMemoryCounters)
            handle = ctypes.windll.kernel32.GetCurrentProcess()
            get_memory_info = getattr(ctypes.windll.kernel32, "K32GetProcessMemoryInfo", None)
            if get_memory_info is None:
                get_memory_info = ctypes.windll.psapi.GetProcessMemoryInfo
            get_memory_info.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessMemoryCounters), wintypes.DWORD]
            get_memory_info.restype = wintypes.BOOL
            if get_memory_info(handle, ctypes.byref(counters), counters.cb):
                return int(counters.WorkingSetSize // 1024)
        except Exception:
            return 0

    if sys.platform.startswith("linux"):
        try:
            with open("/proc/self/status", "r", encoding="utf-8") as status:
                for line in status:
                    if line.startswith("VmRSS:"):
                        return int(line.split()[1])
        except OSError:
            return 0

    try:
        import resource

        rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return int(rss if sys.platform != "darwin" else rss / 1024)
    except Exception:
        return 0


def capture_resource() -> ResourceSnapshot:
    return ResourceSnapshot(wall_ns=now_ns(), cpu_ns=cpu_ns(), rss_kib=current_rss_kib())


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise ConnectionError("peer closed connection")
        chunks.extend(chunk)
    return bytes(chunks)


def make_packet(kind: int, seq: int, client_send_ns: int = 0, server_recv_ns: int = 0, server_send_ns: int = 0) -> bytes:
    return PACKET.pack(MAGIC, VERSION, kind, seq, client_send_ns, server_recv_ns, server_send_ns)


def parse_packet(data: bytes) -> tuple[int, int, int, int, int]:
    magic, version, kind, seq, client_send_ns, server_recv_ns, server_send_ns = PACKET.unpack(data)
    if magic != MAGIC or version != VERSION:
        raise ValueError("invalid latency probe packet")
    return kind, seq, client_send_ns, server_recv_ns, server_send_ns


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil((p / 100.0) * len(ordered)) - 1))
    return ordered[index]


def average(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def stddev(values: list[float]) -> float:
    if len(values) < 2:
        return 0.0
    mean = average(values)
    return math.sqrt(sum((value - mean) ** 2 for value in values) / (len(values) - 1))


def color(text: str, code: str, enabled: bool) -> str:
    return f"\033[{code}m{text}\033[0m" if enabled else text


def print_stats(samples: list[Sample], dropped: int, before: ResourceSnapshot, after: ResourceSnapshot, use_color: bool) -> None:
    rtt = [sample.rtt_us for sample in samples]
    one_way = [sample.estimated_one_way_us for sample in samples]
    server_ack = [sample.server_ack_us for sample in samples]
    cpu_delta_ns = max(0, after.cpu_ns - before.cpu_ns)
    wall_delta_ns = max(1, after.wall_ns - before.wall_ns)
    process_cpu = (cpu_delta_ns * 100.0) / wall_delta_ns
    host_cpu = process_cpu / max(1, os.cpu_count() or 1)
    verdict = "PASS" if samples and dropped == 0 else "WARN"

    print()
    print(color("InputFlow cross-host latency probe", "1", use_color))
    print("+----------------------+------------------------------------------+")
    print(f"| Verdict              | {color(verdict, '32' if verdict == 'PASS' else '33', use_color):<40} |")
    print(f"| Samples              | {len(samples):>40} |")
    print(f"| Dropped / timed out  | {dropped:>40} |")
    print("+----------------------+------------------------------------------+")
    print()
    print(color("Latency summary (milliseconds)", "36", use_color))
    print("+----------------------+----------+----------+----------+----------+----------+----------+")
    print("| Metric               |      Avg |      P50 |      P95 |      P99 |      Max |   Jitter |")
    print("+----------------------+----------+----------+----------+----------+----------+----------+")
    for name, values in (
        ("round trip", rtt),
        ("estimated one-way", one_way),
        ("server ACK process", server_ack),
    ):
        values_ms = [value / 1000.0 for value in values]
        print(
            f"| {name:<20} | {average(values_ms):>8.3f} | {percentile(values_ms, 50):>8.3f} | "
            f"{percentile(values_ms, 95):>8.3f} | {percentile(values_ms, 99):>8.3f} | "
            f"{max(values_ms) if values_ms else 0.0:>8.3f} | {stddev(values_ms):>8.3f} |"
        )
    print("+----------------------+----------+----------+----------+----------+----------+----------+")
    print()
    print(color("Client resource usage", "36", use_color))
    print("+-------------------------------+------------+-------+")
    print("| Metric                        |      Value | Unit  |")
    print("+-------------------------------+------------+-------+")
    print(f"| wall time                     | {wall_delta_ns / 1_000_000.0:>10.3f} | ms    |")
    print(f"| process CPU                   | {cpu_delta_ns / 1_000_000.0:>10.3f} | ms    |")
    print(f"| process CPU usage             | {process_cpu:>10.2f} | %     |")
    print(f"| host-normalized CPU usage     | {host_cpu:>10.2f} | %     |")
    print(f"| current resident memory       | {after.rss_kib:>10} | KiB   |")
    print("+-------------------------------+------------+-------+")


def run_server(args: argparse.Namespace) -> int:
    with socket.create_server((args.bind, args.port), reuse_port=False) as server:
        server.listen(1)
        print(f"listening on {args.bind}:{args.port}")
        while True:
            conn, peer = server.accept()
            with conn:
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                print(f"accepted {peer[0]}:{peer[1]}")
                while True:
                    try:
                        kind, seq, client_send_ns, _, _ = parse_packet(recv_exact(conn, PACKET.size))
                    except ConnectionError:
                        break
                    if kind == TYPE_DONE:
                        break
                    if kind != TYPE_PROBE:
                        raise ValueError(f"unexpected packet type {kind}")
                    server_recv_ns = now_ns()
                    server_send_ns = now_ns()
                    conn.sendall(make_packet(TYPE_ACK, seq, client_send_ns, server_recv_ns, server_send_ns))
            print(f"closed {peer[0]}:{peer[1]}")
            if args.once:
                return 0


def run_client(args: argparse.Namespace) -> int:
    samples: list[Sample] = []
    dropped = 0
    timeout = args.timeout_ms / 1000.0
    interval = args.interval_ms / 1000.0

    before = capture_resource()
    try:
        sock = socket.create_connection((args.host, args.port), timeout=timeout)
    except OSError as exc:
        print(f"ERR: failed to connect to {args.host}:{args.port}: {exc}", file=sys.stderr)
        print("Start the server first, then rerun the client command.", file=sys.stderr)
        return 2

    with sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(timeout)
        total = args.count + args.warmup
        for seq in range(total):
            client_send_ns = now_ns()
            sock.sendall(make_packet(TYPE_PROBE, seq, client_send_ns))
            try:
                kind, ack_seq, echoed_send_ns, server_recv_ns, server_send_ns = parse_packet(recv_exact(sock, PACKET.size))
                client_ack_ns = now_ns()
            except (OSError, ConnectionError, ValueError):
                dropped += 1
                continue
            if kind != TYPE_ACK or ack_seq != seq or echoed_send_ns != client_send_ns:
                dropped += 1
                continue
            if seq >= args.warmup:
                rtt_us = (client_ack_ns - client_send_ns) / 1000.0
                samples.append(
                    Sample(
                        seq=seq - args.warmup,
                        rtt_us=rtt_us,
                        estimated_one_way_us=rtt_us / 2.0,
                        server_ack_us=(server_send_ns - server_recv_ns) / 1000.0,
                    )
                )
            if interval > 0:
                time.sleep(interval)
        sock.sendall(make_packet(TYPE_DONE, total))
    after = capture_resource()
    print_stats(samples, dropped, before, after, args.color == "always" or (args.color == "auto" and sys.stdout.isatty()))
    return 0 if samples and dropped == 0 else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Measure ACK-based latency between two machines.")
    subcommands = parser.add_subparsers(dest="command", required=True)

    server = subcommands.add_parser("server", help="Run the responder on one machine.")
    server.add_argument("--bind", default="0.0.0.0")
    server.add_argument("--port", type=int, default=15111)
    server.add_argument("--once", action="store_true", help="Exit after one client run instead of accepting more clients.")
    server.set_defaults(func=run_server)

    client = subcommands.add_parser("client", help="Run the measuring side against a server.")
    client.add_argument("host")
    client.add_argument("--port", type=int, default=15111)
    client.add_argument("--count", type=int, default=500)
    client.add_argument("--warmup", type=int, default=25)
    client.add_argument("--interval-ms", type=float, default=1.0)
    client.add_argument("--timeout-ms", type=float, default=1000.0)
    client.add_argument("--color", choices=("auto", "always", "never"), default="auto")
    client.set_defaults(func=run_client)
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
