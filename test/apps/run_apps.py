#!/usr/bin/env python3
# platform: host-agnostic -- Linux preload; Darwin would need DYLD_INSERT_LIBRARIES
"""Run real programs natively and under libavxemu, one shell command per workload, and compare the
sha256 of what they print (and their exit status).

  run_apps.py --lib LIBAVXEMU [--known FILE] [--modes default,sigill,...] [--work DIR] [--only GLOB]

AVXEMU_APPS_REMOTE: a command prefix (e.g. "ssh avx1-host") that every command is run through, so
the same script can run on a real AVX-only machine; LIBAVXEMU and the programs must exist there.
Case names are apps/<workload>/<mode> ("native" is the reference, never a case). Failures must be
listed in the known-failures file (column 2 is an fnmatch glob), and a listed case that passes fails.
Exit: 0 all as expected, 1 problems, 77 nothing could run.
"""
import argparse, fnmatch, hashlib, os, shlex, subprocess, sys

# Env sets forcing each way an instruction reaches the emulator (names mirror the library's switches).
MODES = {
    "default": "",
    "sigill": "AVXEMU_RELOC=0 AVXEMU_NOCACHE=1",
    "forcetramp": "AVXEMU_FORCETRAMP=1 AVXEMU_NOCACHE=1",
    "nonative": "AVXEMU_NATIVE=0 AVXEMU_MINSPILL=0 AVXEMU_FULLTHUNK=1 AVXEMU_NOCACHE=1",
}

SQLITE = ("create table t(a integer primary key, b integer, c text);"
          "with recursive s(i) as (select 1 union all select i+1 from s where i<200000) "
          "insert into t select i, (i*7919)%1000, printf('%08x', i*2654435761%4294967296) from s;"
          "create index tb on t(b); select b, count(*), sum(a), min(c), max(c) from t group by b order by b;"
          "select c from t order by c desc limit 20;")
DUCK = ("create table t as select range a, (range*7919)%1000 b, md5(range::varchar) c from range(500000);"
        "select b, count(*), sum(a), min(c), max(c) from t group by b order by b;"
        "select a, sum(b) over (order by a rows between 5 preceding and current row) from t order by a limit 50;"
        "select count(*) from t x join t y on x.b = y.a % 1000 where x.a < 2000;")
DUCK_PY = ("import duckdb,sys\nc=duckdb.connect()\nfor q in sys.argv[1].split(';'):\n"
           "    if q.strip():\n        r=c.execute(q)\n        if r.description: print(r.fetchall())\n")
NUMPY = ("import numpy as np,hashlib;r=np.random.default_rng(42);a=r.standard_normal((300,300));b=r.standard_normal((300,300));"
         "o=[a@b,np.sort(a.ravel()),np.fft.fft(a.ravel()).real,np.cumsum(b,axis=0),np.exp(a/5),np.argsort(b,axis=1),"
         "(a.astype(np.float32)*b.astype(np.float32)).sum(0),np.unpackbits((a>0).astype(np.uint8)),np.linalg.norm(a)];"
         "[print(hashlib.sha256(np.ascontiguousarray(x).tobytes()).hexdigest()) for x in o]")
KEY = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
IV = "00112233445566778899aabbccddeeff"
GEN = ("python3 -c \"import sys;sys.stdout.buffer.write(b''.join(((i*2654435761)%4294967296).to_bytes(4,'little')"
       " for i in range(1500000)))\"")


def workloads():
    q = shlex.quote
    return [
        ("zstd-roundtrip", ["zstd"], f"{GEN} | zstd -3 -c | zstd -dc | sha256sum"),
        ("zstd-compress", ["zstd"], f"{GEN} | zstd -19 -T1 -c | sha256sum"),
        ("openssl-sha", ["openssl"], f"{GEN} | openssl dgst -sha256 && {GEN} | openssl dgst -sha512 && {GEN} | openssl dgst -sha1"),
        ("openssl-aes", ["openssl"], f"{GEN} | openssl enc -aes-256-cbc -K {KEY} -iv {IV} | sha256sum && "
                                     f"{GEN} | openssl enc -aes-256-ctr -K {KEY} -iv {IV} | sha256sum && "
                                     f"{GEN} | openssl enc -chacha20 -K {KEY} -iv {IV} | sha256sum"),
        ("sqlite", ["sqlite3"], f"sqlite3 :memory: {q(SQLITE)}"),
        ("numpy", ["python3"], f"python3 -c {q(NUMPY)}"),
        ("duckdb", ["duckdb"], f"duckdb :memory: {q(DUCK)}"),
        ("duckdb-python", ["python3"], f"python3 -c {q(DUCK_PY)} {q(DUCK)}"),
    ]


def sh(cmd, remote, env="", lib=None, timeout=900):
    inner = f"env LD_PRELOAD={shlex.quote(lib)} {env} sh -c {shlex.quote(cmd)}" if lib else cmd
    full = f"{remote} {shlex.quote(inner)}" if remote else inner
    try:
        p = subprocess.run(full, shell=True, capture_output=True, timeout=timeout)
        return p.returncode, hashlib.sha256(p.stdout).hexdigest(), p.stderr.decode(errors="replace")[-300:]
    except subprocess.TimeoutExpired:
        return -9, "timeout", "timeout"


def present(cmd, remote):
    r = subprocess.run((remote + " " if remote else "") + cmd, shell=True, capture_output=True)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", default="")
    ap.add_argument("--known", default="")
    ap.add_argument("--modes", default=",".join(MODES))
    ap.add_argument("--work", default=".")
    ap.add_argument("--only", default="*")
    ap.add_argument("--expected", default="", help="TSV of workload, output sha256 recorded natively on an AVX2 host")
    ap.add_argument("--record", default="", help="run natively only and write that TSV (use on an AVX2 host)")
    a = ap.parse_args()
    remote = os.environ.get("AVXEMU_APPS_REMOTE", "")
    known = []
    if a.known:
        for line in open(a.known):
            if line[0] != "#" and line.strip():
                c = line.rstrip("\n").split("\t")
                if c[1].startswith("apps/"):
                    known.append((c[0], c[1]))
    expected = {}
    if a.expected and os.path.exists(a.expected):
        expected = dict(line.rstrip("\n").split("\t")[:2] for line in open(a.expected) if line.strip() and line[0] != "#")
    recorded = []
    ran = bad = 0
    hits = {k: [0, 0] for k in known}
    os.makedirs(a.work, exist_ok=True)
    for name, tools, cmd in workloads():
        if not fnmatch.fnmatch(name, a.only):
            continue
        if not all(present(f"command -v {t}", remote) for t in tools):
            print(f"SKIP apps/{name}: {' '.join(tools)} not found")
            continue
        if name == "numpy" and not present("python3 -c 'import numpy'", remote):
            print("SKIP apps/numpy: python numpy missing")
            continue
        if name == "duckdb-python" and not present("python3 -c 'import duckdb'", remote):
            print("SKIP apps/duckdb-python: python duckdb module missing")
            continue
        ref = sh(cmd, remote)
        if a.record:
            if ref[0] == 0: recorded.append(f"{name}\t{ref[1]}")
            else: print(f"SKIP apps/{name}: native run fails ({ref[0]})")
            continue
        if ref[0] != 0 and name in expected:
            ref = (0, expected[name], "")        # this CPU cannot run it natively: use the recording from one that can
        elif ref[0] != 0:
            print(f"SKIP apps/{name}: the native run itself fails ({ref[0]}) and no recording exists: {ref[2][-120:]!r}")
            continue
        for mode in a.modes.split(","):
            case = f"apps/{name}/{mode}"
            got = sh(cmd, remote, MODES[mode], a.lib)
            ok = got[:2] == ref[:2]
            ran += 1
            matched = [k for k in known if fnmatch.fnmatch(case, k[1])]
            for k in matched:
                hits[k][0] += 1
                hits[k][1] += (not ok)
            if not ok and not matched:
                bad += 1
                print(f"FAIL {case}: exit {got[0]} (native {ref[0]}), output {'differs' if got[1] != ref[1] else 'same'}; stderr: {got[2][-200:]!r}")
            else:
                print(f"{'expected-fail' if not ok else 'ok'} {case}")
    if a.record:
        open(a.record, "w", newline="\n").write("# apps output hashes recorded natively on an AVX2 host (run_apps.py --record)\n" + "\n".join(recorded) + "\n")
        print(f"recorded {len(recorded)} workloads to {a.record}")
        return 0 if recorded else 77
    for k, (r, f) in hits.items():
        if r and not f:
            print(f"XPASS known failure {k[0]} now passes: remove it")
            bad += 1
    print(f"apps: {ran} runs, {bad} problems")
    return 77 if not ran else (1 if bad else 0)


sys.exit(main())
