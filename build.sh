#!/bin/bash
# Build recipe of this solver, as used for every build of the paper's runs.
# Run it from this directory:  bash build.sh <output directory>   ->  <output directory>/solver
set -e
set -o pipefail
CXX=${CXX:-g++}
command -v "$CXX" > /dev/null || { echo "no C++ compiler ($CXX) on PATH"; exit 1; }
[ -f host_main.cpp ] || { echo "host_main.cpp is missing"; exit 1; }
[ -f gc/lib/solution.cpp ] || { echo "gc/lib/solution.cpp is missing"; exit 1; }
[ -f params.txt ] || { echo "params.txt is missing (one line: the search to run and its options, e.g.  MBA* c 1 f 1.5)"; exit 1; }
python3 - <<'PY' || exit 1
import re, sys
t = [l.strip() for l in open("params.txt").read().splitlines() if l.strip()]
if len(t) != 1 or not re.match(r"^(MBA\*|BS|BSS|A\*|DFS|DPA\*)( [a-z] -?[0-9]+(\.[0-9]+)?)*$", t[0]):
    sys.exit("params.txt must hold ONE line: a search name (MBA*, BS, BSS, A*, DFS, DPA*) followed by pairs `<letter> <number>` (found: %s)" % t)
PY
SRCS="host_main.cpp $(ls gc/lib/*.cpp gc/ub_*/*.cpp)"
if [ "$2" = check ]; then                   # compile errors only, no binary
  "$CXX" -std=gnu++14 -O1 -w -fsyntax-only -I. $SRCS 2>&1 | tail -n 150
  exit 0
fi
"$CXX" -std=gnu++14 -O3 -march=native -DNDEBUG -w -pthread -I. $SRCS -o "$1/prog" 2>&1 | tail -n 150
cp params.txt "$1/params.txt"
cat > "$1/solver" <<'LAUNCHER'
#!/usr/bin/env python3
"""solver <instance.gfam> <solution> -t <seconds> -seed <int> [-keep]: writes the csv files of the instance (for a
family: the instance that the seed selects) into <solution>.inst/, runs the program on them with the search of
params.txt in ONE thread, and removes the csv files again (-keep leaves them)."""
import json
import os
import shutil
import subprocess
import sys

import random as _random

_QL = [0.0, 0.10, 0.25, 0.50, 0.75, 0.90, 1.0]
_LONG = [350.0, 760.0, 980.0, 1230.0, 1720.0, 2010.0, 3200.0]       # long side of an item, mm
_ASP = [1.0, 1.25, 1.6, 2.25, 3.0, 4.0, 10.0]                       # long side / short side
_DEF_N = [0.20, 0.20, 0.20, 0.15, 0.10, 0.07, 0.04, 0.02, 0.02]     # defects on a plate: 0 .. 8
_DEF_S = [(1, 0.40), (2, 0.25), (3, 0.15), (4, 0.08), (5, 0.05), (8, 0.04), (15, 0.02), (40, 0.01)]
PARAM_ROWS = ["NAME;VALUE", "nPlates;100", "widthPlates;6000", "heightPlates;3210", "min1Cut;100", "max1Cut;3500",
              "min2Cut;100", "minWaste;20"]


def _q(table, u):
    for i in range(1, len(_QL)):
        if u <= _QL[i]:
            return table[i - 1] + (table[i] - table[i - 1]) * (u - _QL[i - 1]) / (_QL[i] - _QL[i - 1])
    return table[-1]


def _pick(rng, weights):
    u, acc = rng.random(), 0.0
    for i, w in enumerate(weights):
        acc += w
        if u < acc:
            return i
    return len(weights) - 1


def _between(rng, lo, hi):
    return int(lo) + int(rng.random() * (int(hi) - int(lo) + 1))


def gen_instance(fam, seed):
    """(batch rows, defect rows, parameter rows) of instance number `seed` of a family; rows are csv lines."""
    rng = _random.Random("%s/%d" % (fam["name"], int(seed)))
    n = _between(rng, fam["n"][0], fam["n"][1])
    scale = float(fam.get("scale", 1.0))
    dup = float(fam.get("dup", 0.5))
    sizes = []
    for i in range(n):
        if sizes and rng.random() < dup:                 # a size that is in the order book already; early sizes more often
            k = int(len(sizes) * rng.random() ** 2)
            sizes.append(sizes[k])
            continue
        long_ = min(3200.0, max(300.0, _q(_LONG, rng.random()) * scale))
        short = max(120.0, long_ / _q(_ASP, rng.random()))
        a, b = int(round(long_)), int(round(short))
        sizes.append((a, b) if rng.random() < 0.5 else (b, a))
    # stacks: a given number of them, or long chains for a share of the items and single items for the rest
    if fam.get("stacks"):
        k = _between(rng, fam["stacks"][0], fam["stacks"][1])
        cuts = sorted(_between(rng, 1, n - 1) for _ in range(k - 1)) if n > 1 else []
        lens = [b - a for a, b in zip([0] + cuts, cuts + [n]) if b > a]
    else:
        in_chains = int(round(n * float(fam.get("chain_share", 0.5))))
        lens = []
        while in_chains > 0:
            L = min(in_chains, _between(rng, fam["chain_len"][0], fam["chain_len"][1]))
            lens.append(L)
            in_chains -= L
        lens += [1] * (n - sum(lens))
        for i in range(len(lens) - 1, 0, -1):             # the stacks in a random order
            j = int(rng.random() * (i + 1))
            lens[i], lens[j] = lens[j], lens[i]
    batch = ["ITEM_ID;LENGTH_ITEM;WIDTH_ITEM;STACK;SEQUENCE"]
    it = 0
    for s, L in enumerate(lens):
        for q in range(L):
            batch.append("%d;%d;%d;%d;%d" % (it, sizes[it][0], sizes[it][1], s, q + 1))
            it += 1
    defects = ["DEFECT_ID;PLATE_ID;X;Y;WIDTH;HEIGHT"]
    d = 0
    for p in range(100):
        for _ in range(_pick(rng, _DEF_N)):
            w = _DEF_S[_pick(rng, [x[1] for x in _DEF_S])][0]
            h = _DEF_S[_pick(rng, [x[1] for x in _DEF_S])][0]
            w, h = max(1, int(w * (0.5 + rng.random()))), max(1, int(h * (0.5 + rng.random())))
            x, y = int(rng.random() * (6000 - w)), int(rng.random() * (3210 - h))
            defects.append("%d;%d;%d.0;%d.0;%d.0;%d.0" % (d, p, x, y, w, h))
            d += 1
    return batch, defects, list(PARAM_ROWS)


def materialise(fam, seed):
    """The three csv files of an instance as lists of lines (header first)."""
    if fam.get("kind") == "fixed":
        return list(fam["batch"]), list(fam["defects"]), list(fam["param"])
    return gen_instance(fam, seed)


def write_instance(directory, prefix, fam, seed):
    batch, defects, param = materialise(fam, seed)
    for name, rows in ((prefix + "_batch.csv", batch), (prefix + "_defects.csv", defects), ("global_param.csv", param)):
        with open(os.path.join(directory, name), "w", newline="") as f:
            f.write("".join(r + "\r\n" for r in rows))

here = os.path.dirname(os.path.abspath(__file__))
a = sys.argv[1:]
inst, sol = a[0], a[1]
T = a[a.index("-t") + 1]
seed = a[a.index("-seed") + 1]
with open(inst, encoding="utf-8") as f:
    fam = json.load(f)
d = sol + ".inst"
shutil.rmtree(d, ignore_errors=True)
os.makedirs(d)
write_instance(d, "X", fam, int(seed))
algo = [l.strip() for l in open(os.path.join(here, "params.txt")).read().splitlines() if l.strip()][0]
rc = subprocess.call([os.path.join(here, "prog"), os.path.join(d, "X"), T, seed, sol, algo])
if "-keep" not in a:
    shutil.rmtree(d, ignore_errors=True)
sys.exit(rc)
LAUNCHER
chmod +x "$1/solver"
