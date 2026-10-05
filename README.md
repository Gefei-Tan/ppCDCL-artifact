# ppCDCL

ppCDCL is a two-party privacy-preserving SAT solver. It runs conflict-driven
clause learning (CDCL) inside secure two-party computation. Two parties use it
to solve the conjunction of their private CNF formulas without revealing the
formulas to each other.

This repository contains the prototype implementation, the benchmark formulas,
and the measurement scripts for the paper
> Gefei Tan, Wenhao Zhang, Timos Antonopoulos, Ruzica Piskac, Xiao Wang, and
> Ning Luo. *Towards Practical Privacy-Preserving SAT Solving.* ACM CCS 2026.



## Repository layout

- `src/`: the secure solver. `src/cdcl.h` contains the top level algorithm.
- `emp-dpf/`: the ORAM implementation.
- `plaintext_cdcl/`: a plaintext version of the ppCDCL.
- `ppsat/`: the ppSAT solver (Luo et al., USENIX Security 2022), used as the
  baseline.
- `scripts/`: Python scripts for preprocessing, cost estimation, and
  end-to-end runs.
- `benchmarks/`: the CNF formulas of the evaluation.

## Requirements

- A C++17 compiler, CMake 3.16 or later, OpenSSL, and Python 3.
- EMP-toolkit, and CaDiBack (a backbone extractor built on the CaDiCaL SAT
  solver), set up as described below.

### EMP-toolkit

ppCDCL needs emp-tool and emp-ot at the commits below. 

```bash
git clone https://github.com/emp-toolkit/emp-tool.git
cd emp-tool
git fetch origin 27eb2d4cc3a2846002d151931087ec326cb4696e
git checkout 27eb2d4cc3a2846002d151931087ec326cb4696e
cmake . -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && make -j4 && sudo make install
cd ..

git clone https://github.com/emp-toolkit/emp-ot.git
cd emp-ot
git fetch origin 0342af547fa80477e866c56b5e2632315ae51721
git checkout 0342af547fa80477e866c56b5e2632315ae51721
cmake . -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && make -j4 && sudo make install
cd ..
```

The ppSAT baseline also needs emp-sh2pc and a different branch of emp-tool. The second emp-tool goes under the prefix `/usr/local/emp-tool-master`,
where `ppsat/CMakeLists.txt` looks for it, so it does not replace the first
one. If you do not need ppSAT, skip this step and build with
`-DPPCDCL_BUILD_PPSAT=OFF`.

```bash
git clone https://github.com/emp-toolkit/emp-sh2pc.git
cd emp-sh2pc
git fetch origin a727e46defe15e3e7cfb40b62fc16b65b56acf1f
git checkout a727e46defe15e3e7cfb40b62fc16b65b56acf1f
cmake . && make -j4 && sudo make install
cd ..

git clone https://github.com/emp-toolkit/emp-tool.git emp-tool-ppsat
cd emp-tool-ppsat
git fetch origin 11093a7d2160e7e7a4dcae3ffd9e6935bf2b8c1c
git checkout 11093a7d2160e7e7a4dcae3ffd9e6935bf2b8c1c
cmake . -DCMAKE_INSTALL_PREFIX=/usr/local/emp-tool-master && make -j4 && sudo make install
cd ..
```

### CaDiBack

The scripts run CaDiBack during preprocessing. They expect the binary
`cadiback` in the repository root; `--preprocess-backbone-bin` sets another
path. The CaDiBack build expects a built CaDiCaL in a sibling directory named
`cadical`:

```bash
git clone https://github.com/arminbiere/cadical.git
git clone https://github.com/arminbiere/cadiback.git
(cd cadical && ./configure && make)
(cd cadiback && ./configure && make)
cp cadiback/cadiback /path/to/ppCDCL/   # the root of this repository
```

## Build

Run all commands in this README from the repository root. The binaries read
`emp-dpf/files/` and write to `data/` by relative path.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
mkdir -p data
```

To build without ppSAT, add `-DPPCDCL_BUILD_PPSAT=OFF` to the first command.

## Solve one formula

The following command runs the secure solver on `uf20-01.cnf`. `./run` starts
the given binary twice on this machine, as party 1 and as party 2, with the
same arguments.

```bash
./run ./build/bin/test_ppcdcl_solver_e2e ./uf20-01.cnf sat 1000000 \
  --public-early-exit \
  --max-watchlist 14 --max-conflict-literals 10 --max-conflict-clauses 4 \
  --uip-cap 30 --decision-delay 10 --conflict-delay 10 --threads 2
```

## Estimate the cost on benchmarks

```bash
python3 scripts/compare.py --dir ./benchmarks/Haplotype --no-ppsat \
  --fixed-max-conflict-clauses 10000 --uip-loop-cap 20 \
  --bin-bench ./build/bin/test_good_bench \
  --out-csv results/haplotype.csv

python3 scripts/compare.py --dir ./benchmarks/SATLIB --no-ppsat \
  --fixed-max-conflict-clauses 10000 --uip-loop-cap 140 \
  --bin-bench ./build/bin/test_good_bench \
  --out-csv results/satlib.csv
```

# Use of Large language models
Large language models were used to help refactor, reorganize, and document the code to prepare this public version.