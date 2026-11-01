"""Produces the three measurements of REPORT_M1.md (SPEC §M1.10).

Usage (from the repo root, after building):
    python bench/report_m1.py [path/to/storage.exe]

It calls the `storage` executable with --json, checks the numbers, runs every
measurement twice (the counters must be identical, GLOBAL §0.2), and prints
the Markdown tables. Everything it creates goes in bench/out/, which is
deleted at the start, so every run starts from nothing.

Traces: if data/traces/ holds .txt files (the published ones), they are used.
Until then, five seeded stand-ins are generated; trace 3 is exactly the
spec's (pages 0-79 scanned ten times), the other four are synthetic.
"""

import json
import random
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "bench" / "out"
STORAGE = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build" / "apps" / "storage.exe")


def run(*args):
    """Runs `storage args... --json` and returns the parsed JSON. Stops the
    script, printing the command and its error, on a non-zero exit code."""
    cmd = [STORAGE, *map(str, args), "--json"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"failed: {' '.join(cmd)}\n{result.stderr}")
    return json.loads(result.stdout)


def run_twice(*args):
    """Runs the same command twice; both outputs must be identical."""
    first, second = run(*args), run(*args)
    assert first == second, f"counters differ between two runs: {args}"
    return first


def table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    lines += ["| " + " | ".join(str(v) for v in row) + " |" for row in rows]
    return "\n".join(lines)


def write_lines(path, lines):
    path.write_text("".join(f"{line}\n" for line in lines))
    return path


def load_table(db, name, schema, rows):
    """Creates table `name` in `db` and loads `rows` (lists of values) into it."""
    csv = write_lines(OUT / f"{name}.csv", [",".join(map(str, r)) for r in rows])
    subprocess.run([STORAGE, "create-table", "--db", db, "--name", name, "--schema", schema],
                   check=True, capture_output=True)
    subprocess.run([STORAGE, "load", "--db", db, "--table", name, "--file", str(csv)],
                   check=True, capture_output=True)


# 1. Capacity: computed floor(4088 / (L + 4)) vs measured slot_count of page 1.
def capacity(db):
    cases = [  # name, schema, one row, record length L in bytes
        ("all_int", "a:INT,b:INT,c:INT", [1, 2, 3], 12),
        ("short_varchar", "a:INT,b:VARCHAR(20),c:INT", [42, "bob", 7], 4 + 2 + 3 + 4),
        ("long_varchar", "a:INT,b:VARCHAR(200)", [1, "x" * 200], 4 + 2 + 200),
    ]
    rows = []
    for name, schema, row, length in cases:
        load_table(db, name, schema, [row] * 1000)  # enough to fill page 1 and more
        computed = 4088 // (length + 4)
        measured = run_twice("layout", "--db", db, "--table", name, "--page", 1)["slot_count"]
        assert measured == computed, f"{name}: measured {measured}, computed {computed}"
        rows.append([f"`{schema}`", length, computed, measured])
    return table(["Schema", "L (bytes)", "Computed", "Measured"], rows)


# 2. Policies: every trace x B in 4, 16, 64, 256 x LRU and CLOCK.
def stand_in_traces():
    rng = random.Random(3005)  # fixed seed: the same files on every run
    hot = list(range(40))  # 20% of 200 pages
    mix = []
    for _ in range(5):
        mix += list(range(100))                                  # a scan of 100 pages
        mix += [rng.choice(range(300, 320)) for _ in range(100)]  # lookups in 20 hot pages
    traces = {
        "trace1 (synthetic: uniform random, 200 pages)": [rng.randrange(200) for _ in range(1000)],
        "trace2 (synthetic: 80% of accesses to 20% of 200 pages)":
            [rng.choice(hot) if rng.random() < 0.8 else rng.randrange(40, 200) for _ in range(1000)],
        "trace3 (spec: 80-page scan x 10)": list(range(80)) * 10,
        "trace4 (synthetic: loop over 40 pages)": list(range(40)) * 25,
        "trace5 (synthetic: scans mixed with hot lookups)": mix,
    }
    folder = OUT / "traces"
    folder.mkdir()
    return {label: write_lines(folder / f"trace{i}.txt", ids)
            for i, (label, ids) in enumerate(traces.items(), start=1)}


def policies(db):
    # Self-test first: the spec's worked example (§M1.6) must reproduce.
    spec = write_lines(OUT / "spec_trace.txt", [1, 2, 3, 1, 4, 2])
    for policy, expected in (("lru", (1, 5, 2)), ("clock", (2, 4, 1))):
        r = run("trace", "--db", db, "--file", spec, "--frames", 3, "--policy", policy)
        assert (r["hits"], r["misses"], r["evictions"]) == expected, f"spec trace, {policy}: {r}"

    real = sorted((ROOT / "data" / "traces").glob("*.txt"))
    traces = {p.stem: p for p in real} if real else stand_in_traces()
    tables = []
    for policy in ("lru", "clock"):
        rows = []
        for label, path in traces.items():
            length = sum(1 for line in path.read_text().splitlines() if line.strip())
            for frames in (4, 16, 64, 256):
                r = run_twice("trace", "--db", db, "--file", path, "--frames", frames, "--policy", policy)
                assert r["hits"] + r["misses"] == length and r["reads"] == r["misses"] and r["writes"] == 0
                rows.append([label, frames, r["hits"], r["misses"], r["evictions"],
                             f"{r['hits'] / length:.1%}"])
        tables.append(f"**{policy.upper()}**\n\n"
                      + table(["Trace", "B", "Hits", "Misses", "Evictions", "Hit rate"], rows))
    return "\n\n".join(tables)


# 3. Scan cost: a table of exactly 500 data pages (255 all-INT rows per page).
def scan_cost(db):
    load_table(db, "big", "a:INT,b:INT,c:INT", [[i, i, i] for i in range(500 * 255)])
    rows = []
    for frames in (2, 8, 64):
        stats = run_twice("scan", "--db", db, "--table", "big", "--frames", frames)["stats"]
        assert stats["writes"] == 0
        rows.append([frames, stats["reads"], stats["hits"], stats["misses"], stats["evictions"]])
    return table(["B", "Reads", "Hits", "Misses", "Evictions"], rows)


if __name__ == "__main__":
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    db = str(OUT / "db")
    print("## Capacity\n\n" + capacity(db) + "\n")
    print("## Policies\n\n" + policies(db) + "\n")
    print("## Scan cost\n\n" + scan_cost(db))
