"""Client for the ESPHome Device Builder's WebSocket API (no web page needed).

The dashboard (ESPHome Device Builder 1.12, at http://10.0.0.82:6052 by default)
speaks JSON over ws://HOST:6052/ws: on connect the server sends its info
({"server_version": ...}); each request is {"command", "message_id", "args"} and
the reply carries the same message_id with "result" or "error". Long jobs
(compile/install) report job_* events. Command names are the dashboard's own,
e.g. devices/list, devices/get_config, devices/update_config, firmware/install.

Run with the project's ESPHome venv (needs `websockets`):
  .venv-esphome/Scripts/python.exe esphome/dashboard.py list
  .venv-esphome/Scripts/python.exe esphome/dashboard.py get atmos.yaml > atmos.yaml
  .venv-esphome/Scripts/python.exe esphome/dashboard.py put atmos.yaml local-copy.yaml
  .venv-esphome/Scripts/python.exe esphome/dashboard.py install atmos.yaml   # OTA, waits for compile+upload
  .venv-esphome/Scripts/python.exe esphome/dashboard.py logs atmos.yaml 60   # device log for 60 s
  .venv-esphome/Scripts/python.exe esphome/dashboard.py call devices/list '{}'
Set ATMOS_DASHBOARD=host:port to use another server.
"""
import asyncio, itertools, json, os, re, sys

import websockets

ANSI = re.compile(r"(\\033|\x1b)\[[0-9;]*m")  # colour codes, escaped or raw
URL = f"ws://{os.environ.get('ATMOS_DASHBOARD', '10.0.0.82:6052')}/ws"


class Dashboard:
    def __init__(self, url=URL):
        self.url, self._ids = url, itertools.count(1)

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=None)
        self.info = json.loads(await self.ws.recv())
        return self

    async def __aexit__(self, *exc):
        await self.ws.close()

    async def send(self, command, args=None):
        mid = str(next(self._ids))
        msg = {"command": command, "message_id": mid}
        if args:
            msg["args"] = args
        await self.ws.send(json.dumps(msg))
        return mid

    async def call(self, command, args=None, timeout=60):
        """Send one command and return its result (raises on an error reply)."""
        mid = await self.send(command, args)
        while True:
            m = json.loads(await asyncio.wait_for(self.ws.recv(), timeout))
            if m.get("message_id") != mid:
                continue
            if "error" in m:
                raise RuntimeError(f"{command}: {m['error']}")
            return m.get("result")

    async def events(self, timeout=1800):
        """Yield every message the server pushes (job events, streams)."""
        while True:
            yield json.loads(await asyncio.wait_for(self.ws.recv(), timeout))

    async def wait_jobs(self, configuration, since, out=sys.stdout, every=5):
        """Wait for the jobs `configuration` queued after `since` (an install queues
        a compile, then an upload depending on it); print each one's progress and,
        on failure, its output tail. Returns True when all completed."""
        seen = {}
        while True:
            jobs = [j for j in await self.call("firmware/get_jobs")
                    if j["configuration"] == configuration and j["created_at"] > since]
            for j in jobs:
                state = (j["status"], j.get("progress"))
                if seen.get(j["job_id"]) != state:
                    seen[j["job_id"]] = state
                    print(f"{j['job_type']:8} {j['status']:10} {j.get('progress') or 0:3}%", file=out, flush=True)
            if jobs and all(j["status"] in ("completed", "failed", "cancelled") for j in jobs):
                for j in jobs:
                    if j["status"] != "completed":
                        print("\n".join(j.get("output") or [])[-4000:], j.get("error") or "", file=out)
                return all(j["status"] == "completed" for j in jobs)
            await asyncio.sleep(every)

    async def logs(self, configuration, seconds, out=sys.stdout):
        """Stream the device's log through the dashboard for `seconds`."""
        mid = await self.send("devices/logs", {"configuration": configuration, "port": "OTA"})
        loop = asyncio.get_running_loop()
        end = loop.time() + seconds
        try:
            while (left := end - loop.time()) > 0:
                try:
                    m = json.loads(await asyncio.wait_for(self.ws.recv(), left))
                except asyncio.TimeoutError:
                    break
                if m.get("message_id") != mid:
                    continue
                data = m.get("data")  # {"event": "output", "data": "<log line>"}
                if isinstance(data, str):
                    print(ANSI.sub("", data), file=out, flush=True)
                else:
                    print(json.dumps(m)[:500], file=out, flush=True)
        finally:
            await self.call("devices/stop_stream", {"stream_id": mid})


async def main(argv):
    if not argv:
        print(__doc__)
        return 2
    cmd, rest = argv[0], argv[1:]
    async with Dashboard() as d:
        if cmd == "list":
            for dev in (await d.call("devices/list")).get("configured", []):
                print(f"{dev['configuration']:28} {dev.get('state', '?'):8} {dev.get('ip', ''):15} {dev.get('deployed_version', '')}")
        elif cmd == "get":
            sys.stdout.write(await d.call("devices/get_config", {"configuration": rest[0]}))
        elif cmd == "put":
            content = open(rest[1], encoding="utf-8").read()
            await d.call("devices/update_config", {"configuration": rest[0], "content": content})
            print(f"saved {rest[0]}")
        elif cmd == "install":
            port = rest[1] if len(rest) > 1 else "OTA"
            jobs = await d.call("firmware/get_jobs") or []
            since = max((j["created_at"] for j in jobs), default="")
            await d.call("firmware/install", {"configuration": rest[0], "port": port,
                                              "force_local": False, "bootloader": False})
            ok = await d.wait_jobs(rest[0], since)
            print("== installed" if ok else "== failed")
            return 0 if ok else 1
        elif cmd == "logs":
            await d.logs(rest[0], float(rest[1]) if len(rest) > 1 else 30)
        elif cmd == "call":
            print(json.dumps(await d.call(rest[0], json.loads(rest[1]) if len(rest) > 1 else None,
                                          timeout=600), indent=2))
        else:
            print(__doc__)
            return 2
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main(sys.argv[1:])))
