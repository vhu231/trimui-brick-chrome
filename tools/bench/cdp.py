#!/usr/bin/env python3
"""GPU status and frame-rate benchmarks over the DevTools protocol.

Runs inside the chroot, against the Chrome that brick-chrome-start launched
(remote debugging on port 9222):

  cdp.py 9222 gpu                         SystemInfo.getInfo: feature status, renderer
  cdp.py 9222 bench file:///...html [S]   open the page in a new tab, run its
                                          window.__bench(S) and print the result
  cdp.py 9222 eval JS                     evaluate JS in the first tab

bench reports requestAnimationFrame rate and frame-interval percentiles, plus
how busy the CPUs were during the run.  Run each page once to warm up shader
caches before trusting the numbers.
"""
import json, re, sys, time, urllib.request
import websocket

PORT = int(sys.argv[1])
CMD = sys.argv[2]


def http(path):
    for _ in range(120):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{PORT}{path}", timeout=5) as r:
                return json.loads(r.read())
        except Exception:
            time.sleep(0.5)
    raise SystemExit("devtools endpoint not reachable")


class Conn:
    def __init__(self, url):
        self.ws = websocket.create_connection(url, suppress_origin=True, timeout=180)
        self.n = 0

    def call(self, method, params=None, session=None):
        self.n += 1
        msg = {"id": self.n, "method": method, "params": params or {}}
        if session:
            msg["sessionId"] = session
        self.ws.send(json.dumps(msg))
        while True:
            r = json.loads(self.ws.recv())
            if r.get("id") == self.n:
                if "error" in r:
                    raise RuntimeError(f"{method}: {r['error']}")
                return r["result"]


def browser():
    return Conn(http("/json/version")["webSocketDebuggerUrl"])


def gpu():
    c = browser()
    info = c.call("SystemInfo.getInfo")["gpu"]
    print("devices:", json.dumps(info.get("devices"), ensure_ascii=False))
    print("featureStatus:")
    for k, v in sorted(info.get("featureStatus", {}).items()):
        print(f"  {k:40s} {v}")
    aux = info.get("auxAttributes", {})
    for k in ("glRenderer", "glVendor", "glVersion", "glImplementationParts",
              "displayType", "skiaBackendType", "vulkanVersion",
              "gpuCompositingDisabled", "passthroughCmdDecoder"):
        if k in aux:
            print(f"aux.{k} = {aux[k]}")
    print("workarounds:", ", ".join(info.get("driverBugWorkarounds", [])))


def attach_page(c, url=None):
    if url:
        tid = c.call("Target.createTarget", {"url": url})["targetId"]
    else:
        tid = next(t["targetId"] for t in c.call("Target.getTargets")["targetInfos"]
                   if t["type"] == "page")
    s = c.call("Target.attachToTarget", {"targetId": tid, "flatten": True})["sessionId"]
    return tid, s


def evaluate(c, s, expr, timeout=120):
    r = c.call("Runtime.evaluate", {"expression": expr, "awaitPromise": True,
                                    "returnByValue": True, "timeout": timeout * 1000}, s)
    if "exceptionDetails" in r:
        return {"exception": r["exceptionDetails"].get("exception", {}).get("description")}
    return r["result"].get("value")


def cpu_times():
    return [int(x) for x in open("/proc/stat").readline().split()[1:]]


def bench(url, secs):
    c = browser()
    tid, s = attach_page(c, url)
    for _ in range(240):
        st = evaluate(c, s, "document.readyState + ':' + (typeof window.__bench)")
        if st == "complete:function":
            break
        time.sleep(0.25)
    time.sleep(1.0)
    before = cpu_times()
    res = evaluate(c, s, f"window.__bench({secs})", timeout=secs + 60)
    after = cpu_times()
    busy = [a - b for a, b in zip(after, before)]
    total = sum(busy)
    idle = busy[3] + busy[4]
    if isinstance(res, dict):
        res["cpu_busy_pct"] = round(100.0 * (total - idle) / total) if total else None
        res["cores"] = len([l for l in open("/proc/stat") if re.match(r"cpu\d", l)])
    print(json.dumps(res, ensure_ascii=False))
    c.call("Target.closeTarget", {"targetId": tid})


if CMD == "gpu":
    gpu()
elif CMD == "bench":
    bench(sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else 6)
elif CMD == "eval":
    c = browser()
    _, s = attach_page(c)
    print(json.dumps(evaluate(c, s, sys.argv[3]), ensure_ascii=False))
