# OSS_HLS_Bench

Benchmark and comparison flow for LegUp, Bambu/PANDA, Dynamatic, Vitis HLS, and OpenHLS/VibeHLS.

## Repository Layout

```text
benchmarks are currently under:
  osshls_bench/kernels/       Shared C/C++ kernels and testbenches

runner code:
  hls_bench/                  Shared Python runners, reporting, harnesses, and paths
  hls_bench/cli.py            Main command-line entry point
  hls_bench/legup_runner.py   LegUp batch runner and report parser

tool entry points:
  osshls_bench/tools/legup/       LegUp wrapper
  osshls_bench/tools/panda/       Bambu/PANDA wrapper
  osshls_bench/tools/dynamatic/   Dynamatic wrapper and scripts
  osshls_bench/tools/openhls/     OpenHLS wrapper
  osshls_bench/tools/vitis/       Vitis HLS wrapper

toolchain:
  legup-4.0/                  Local LegUp 4.0 source and vendor toolchain
  docker/legup/Dockerfile     Reproducible LegUp image definition
  hls_bench/scripts/          LegUp and Dynamatic shell entry points

timing and documentation:
  hls_bench/generate_artix7_legup_config.py  Generate LegUp Artix-7 timing config
  legup-4.0-doc.pdf                LegUp reference documentation
  AGENTS.md                        Repository-specific agent instructions

generated data:
  artifacts/                  Current run logs, reports, and tool work trees
```

## Main Commands

Run the default multi-tool benchmark set:

```bash
python3 -m hls_bench.cli parallel --tools legup bambu dynamatic vitis openhls
```

Run selected benchmarks:

```bash
python3 -m hls_bench.cli parallel \
  --benchmarks dotprod spmv matmul \
  --tools legup openhls
```

Run one tool directly:

```bash
python3 -m hls_bench.cli openhls --bench dotprod --run-simulation --run-synthesis
python3 -m hls_bench.cli legup -- --bench dotprod --run-simulation
```

List configured tools and their normalized report paths:

```bash
python3 -m hls_bench.cli tools
```

## Artifact Layout

New runs are written below:

```text
artifacts/
  runs/<timestamp>/           Parallel logs, status, and tables
  reports/                    Latest published comparison tables and figures
  tool-work/<tool>/           Per-tool JSON, RTL, synthesis, and simulation data
```

Paths can be relocated without changing source files:

```bash
HLS_BENCHMARKS_ROOT=/path/to/kernels \
HLS_ARTIFACTS_ROOT=/path/to/artifacts \
python3 -m hls_bench.cli parallel
```

## Comparison Rules

- Cycle counts must come from execution-backed RTL or tool simulation artifacts.
- Area and timing must come from post-synthesis reports.
- Missing metrics remain missing; estimates are not substituted into comparison tables.
- Every tool must use the shared benchmark kernel and equivalent testbench input.

## OpenHLS Directives

OpenHLS automatically creates typed DSE directives for staged kernels:

- Every discovered loop receives pipeline enable with `II=1`.
- Every pointer/array kernel argument receives a `ram_1p` resource directive.
- DSE provenance is stored in the generated manifest and `.dse.directives.json`.

Disable this behavior for a baseline run with:

```bash
python3 -m hls_bench.cli openhls --bench dotprod --no-dse
```

## LegUp

The Artix-7 LegUp flow generates `config.tcl` from the OpenHLS timing database. It enables `PIPELINE_ALL` and uses the LegUp Docker image defined by `docker/legup/Dockerfile`.

Build the image after changing LegUp sources:

```bash
docker build -t legup14:latest -f docker/legup/Dockerfile .
```

The vendor tree is intentionally kept separate from benchmark and reporting code.
