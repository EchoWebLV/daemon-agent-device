#!/usr/bin/env python3
"""Smoke test: configure 5 scheduled tasks, trigger each, verify each succeeds.

Usage:  python3 tools/smoke_tasks.py [host]
"""

import json
import sys
import time
import urllib.request
import urllib.error

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.5.47"
BASE = f"http://{HOST}"
TIMEOUT = 8.0
RUN_TIMEOUT = 90.0


def http(method, path, body=None):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(f"{BASE}{path}", data=data, method=method,
                                 headers=headers)
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return r.status, r.read().decode("utf-8")


def get_tasks():
    s, b = http("GET", "/api/tasks")
    return json.loads(b)


def set_tasks(tasks):
    s, b = http("POST", "/api/tasks", {"tasks": tasks})
    if s != 200:
        raise RuntimeError(f"set_tasks {s}: {b}")


def trigger(tid):
    s, b = http("POST", f"/api/tasks/run/{tid}")
    return s, b


TASKS = [
    {"id": "smoke_a", "name": "Joke",   "prompt": "Tell me a one-line joke.",          "output": "speak", "enabled": True,
     "schedule": {"kind": "every_hours", "n": 24}},
    {"id": "smoke_b", "name": "Greet",  "prompt": "Say hi in five words or less.",     "output": "speak", "enabled": True,
     "schedule": {"kind": "every_hours", "n": 24}},
    {"id": "smoke_c", "name": "Riddle", "prompt": "Give me a one-line riddle.",        "output": "speak", "enabled": True,
     "schedule": {"kind": "every_hours", "n": 24}},
    {"id": "smoke_d", "name": "Quote",  "prompt": "Share a short motivational quote.", "output": "speak", "enabled": True,
     "schedule": {"kind": "every_hours", "n": 24}},
    {"id": "smoke_e", "name": "Cheer",  "prompt": "Give me a short pep talk.",         "output": "speak", "enabled": True,
     "schedule": {"kind": "every_hours", "n": 24}},
]


def main():
    print(f"== Smoke test against {HOST} ==")
    print(f"-> Setting {len(TASKS)} tasks (overwrites existing list)")
    set_tasks(TASKS)
    cur = get_tasks()
    print(f"   slots used: {cur['slots']['used']}/{cur['slots']['max']}, time_synced={cur['time_synced']}")
    for t in cur["tasks"]:
        print(f"   - {t['id']:10}  {t['name']:6}  next_run={t['next_run']} status={t['last_status']}")

    results = []
    for i, task in enumerate(TASKS, 1):
        tid = task["id"]
        print(f"\n[{i}/{len(TASKS)}] Triggering {tid} ({task['name']!r})...")
        # Snapshot last_run before trigger so we can detect completion.
        before = next((t for t in get_tasks()["tasks"] if t["id"] == tid), None)
        prev_last_run = before["last_run"] if before else 0

        s, b = trigger(tid)
        if s != 200:
            print(f"   trigger FAILED: HTTP {s}: {b}")
            results.append((tid, "trigger_fail", b))
            continue
        print(f"   trigger ok, polling for completion...")

        t0 = time.time()
        done = None
        while time.time() - t0 < RUN_TIMEOUT:
            time.sleep(1.0)
            try:
                cur = get_tasks()
            except Exception as e:
                print(f"   poll err: {e}")
                continue
            t = next((x for x in cur["tasks"] if x["id"] == tid), None)
            if not t:
                continue
            if t["last_run"] > prev_last_run and t["last_status"] in ("ok", "ai_fail", "skipped"):
                done = t
                break
        if not done:
            print(f"   TIMEOUT after {RUN_TIMEOUT}s")
            results.append((tid, "timeout", ""))
            continue
        elapsed = time.time() - t0
        status = done["last_status"]
        reply = done.get("last_reply", "")
        if len(reply) > 80:
            reply = reply[:77] + "..."
        print(f"   {status} in {elapsed:.1f}s: {reply!r}")
        results.append((tid, status, reply))

        # Wait for the worker to fully release before the next trigger so
        # /api/tasks/run/<id> doesn't return 409 busy. The worker still
        # plays back TTS for ~5-15s after last_status flips to "ok".
        print(f"   waiting for device idle...")
        idle_t0 = time.time()
        while time.time() - idle_t0 < 30:
            time.sleep(1.0)
            try:
                # Use a tiny GET to probe; fail fast if device wedged.
                s, b = http("GET", "/api/tasks")
                # The next trigger will return 409 if busy; we can't read
                # creature_busy() over the wire, so just give it 8s breathing
                # room after status flip.
                if time.time() - idle_t0 > 8:
                    break
            except Exception:
                time.sleep(2.0)

    print("\n== Summary ==")
    ok = sum(1 for _, s, _ in results if s == "ok")
    fail = len(results) - ok
    for tid, status, reply in results:
        marker = "PASS" if status == "ok" else "FAIL"
        print(f"  {marker}  {tid:10}  {status:14}  {reply}")
    print(f"\n  {ok}/{len(results)} PASS")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
