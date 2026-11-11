#!/usr/bin/env python3
"""The helper's benchmark command (tools/henge-agent.m, "bench"), against a made-up Henge on this Mac.

Runs build/henge-agent (make build/henge-agent first: without it every test fails) with HENGE_AGENT_TEST_PORT
pointing at a link server here and HENGE_AGENT_TEST_BENCH set, so the steps are short and nothing is put on this
Mac's screen (video, scroll and apps answer "skip test"). Checks the lines Henge would hear: each step's begin and end, KEY=NUMBER measures, a second run
refused while one is under way, the work processes' own answers, and a run whose link went: stopped, and nothing of
it heard on the next link.

    python3 tests/test_agent_bench.py
"""
import os
import platform
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AGENT = os.path.join(ROOT, "build", "henge-agent")


class FakeHenge:
    """Accepts the helper's link (GET /.henge/link, answered 101) and collects the lines it sends on the newest one."""

    def __init__(self):
        self.server = socket.socket()
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.port = self.server.getsockname()[1]
        self.lines = []
        self.cond = threading.Condition()
        self.link = None
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                conn, _ = self.server.accept()
            except OSError:
                return
            threading.Thread(target=self._conn, args=(conn,), daemon=True).start()

    def _conn(self, conn):
        data = b""
        while b"\r\n\r\n" not in data:
            got = conn.recv(4096)
            if not got:
                conn.close()
                return
            data += got
        head, rest = data.split(b"\r\n\r\n", 1)
        if not head.startswith(b"GET /.henge/link"):
            conn.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            conn.close()
            return
        conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: henge-link\r\nConnection: Upgrade\r\n\r\n")
        with self.cond:
            self.link = conn
            self.cond.notify_all()
        buf = rest
        while True:
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                with self.cond:
                    if conn is self.link:
                        self.lines.append(line.decode(errors="replace"))
                        self.cond.notify_all()
            got = conn.recv(4096)
            if not got:
                return
            buf += got

    def wait_link(self, timeout=20):
        with self.cond:
            return self.cond.wait_for(lambda: self.link is not None, timeout)

    def drop(self):
        """Closes the link as Henge going would; the helper links again at once."""
        with self.cond:
            conn, self.link = self.link, None
            self.lines.clear()
        conn.shutdown(socket.SHUT_RDWR)
        conn.close()

    def send(self, line):
        self.link.sendall(line.encode() + b"\n")

    def wait_bench(self, until, timeout=60):
        """The bench lines (without "bench ") once one matches until."""
        with self.cond:
            self.cond.wait_for(lambda: any(until(l[6:]) for l in self.bench_lines()), timeout)
            return [l[6:] for l in self.bench_lines()]

    def bench_lines(self):
        return [l for l in self.lines if l.startswith("bench ")]

    def close(self):
        self.server.close()


def need_agent(test):
    """Fails (not skips) without the helper: a run that skipped everything would look like a pass."""
    test.assertTrue(os.path.exists(AGENT), "build/henge-agent not built: make build/henge-agent first")


class AgentBench(unittest.TestCase):
    def setUp(self):
        need_agent(self)
        self.henge = FakeHenge()
        self.home = tempfile.mkdtemp(prefix="henge-agent-bench-")
        env = dict(os.environ, HENGE_AGENT_TEST_PORT=str(self.henge.port), HENGE_AGENT_TEST_HOME=self.home, HENGE_AGENT_TEST_BENCH="1")
        self.agent = subprocess.Popen([AGENT], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.assertTrue(self.henge.wait_link(), "the helper never linked")

    def tearDown(self):
        self.agent.kill()
        self.agent.wait()
        self.henge.close()

    def test_run(self):
        self.henge.send("bench idle,cpu,video,rosetta,No-Such!")
        self.henge.send("bench idle")  # while the first is under way
        lines = self.henge.wait_bench(lambda l: l == "done" or l.startswith("failed ") and l != "failed busy")
        self.assertIn("failed busy", lines)
        lines = [l for l in lines if l != "failed busy"]
        self.assertEqual(lines[0], "begin idle 0")
        self.assertEqual(lines[1], "end idle ok")
        self.assertEqual(lines[2], "begin cpu 0")
        m = re.fullmatch(r"end cpu ok work_s=([0-9.]+) procs=(\d+)", lines[3])
        self.assertIsNotNone(m, lines[3])
        self.assertEqual(int(m.group(2)), os.cpu_count())
        self.assertEqual(lines[4], "end video skip test")
        rosetta = os.path.exists("/Library/Apple/usr/libexec/oah/libRosettaRuntime")
        if rosetta:
            self.assertEqual(lines[5], "begin rosetta 0")
            self.assertRegex(lines[6], r"^end rosetta ok work_s=[0-9.]+$")
            rest = lines[7:]
        else:
            self.assertEqual(lines[5], "end rosetta skip norosetta")
            rest = lines[6:]
        self.assertEqual(rest, ["end NoSuch skip unknown", "done"])  # only letters and digits of a name come back
        # and once it is over, another run is taken
        self.henge.lines.clear()
        self.henge.send("bench idle")
        self.assertEqual(self.henge.wait_bench(lambda l: l == "done"), ["begin idle 0", "end idle ok", "done"])

    def test_link_gone(self):
        self.henge.send("bench " + ",".join(["cpu"] * 20))
        self.assertIn("begin cpu 0", self.henge.wait_bench(lambda l: l == "begin cpu 0"))
        self.henge.drop()  # mid-run
        self.assertTrue(self.henge.wait_link(), "the helper never linked again")
        # the old run is stopped, so a new one is taken (once it has gone: "failed busy" until then)
        deadline = time.monotonic() + 20
        while True:
            self.henge.send("bench idle")
            lines = self.henge.wait_bench(lambda l: l == "done" or l.startswith("failed "))
            if lines != ["failed busy"] or time.monotonic() > deadline:
                break
            time.sleep(0.5)
            with self.henge.cond:
                self.henge.lines.clear()
        self.assertEqual(lines, ["begin idle 0", "end idle ok", "done"])  # and none of the old run's lines
        time.sleep(0.5)
        self.assertEqual([l[6:] for l in self.henge.bench_lines()], lines)

    def test_refused(self):
        self.henge.send("bench a b")  # a space: not a bench line the helper takes
        self.henge.send("bench idle")
        self.assertEqual(self.henge.wait_bench(lambda l: l == "done"), ["begin idle 0", "end idle ok", "done"])


class Work(unittest.TestCase):
    def setUp(self):
        need_agent(self)

    def test_work(self):
        self.assertEqual(subprocess.run([AGENT, "--bench-work", "1"]).returncode, 0)

    def test_intel_half_only(self):
        if platform.machine() != "arm64":
            self.skipTest("an Intel Mac")
        # the Arm half asked for Intel work: 3, so the Rosetta step knows it didn't run under Rosetta
        self.assertEqual(subprocess.run(["arch", "-arm64", AGENT, "--bench-work", "1", "x86_64"]).returncode, 3)
        archs = subprocess.run(["lipo", "-archs", AGENT], capture_output=True, text=True).stdout.split()
        self.assertEqual(sorted(archs), ["arm64", "x86_64"])


if __name__ == "__main__":
    unittest.main()
