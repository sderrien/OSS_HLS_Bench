# Docker Toolchains

The repository exposes one container environment per HLS tool.  All services use
the same checked-out benchmarks and write results to the same `artifacts/` tree.

## Build

Build the portable runner images:

```bash
docker compose build bambu dynamatic openhls vitis
```

Build the legacy LegUp image separately from the repository root.  The root is
kept as the build context because the image copies `legup-4.0/`:

```bash
docker compose --profile legup build legup
# equivalent direct command:
docker build -t legup14:latest -f docker/legup/Dockerfile .
```

## External installations

The following installations are not copied into images:

- `DYNAMATIC_ROOT` points to a built Dynamatic tree.
- `OPENHLS_ROOT` points to an OpenHLS/VibeHLS tree.
- `BAMBU_ROOT` points to a Bambu installation.
- `VIVADO_ROOT` points to the AMD/Xilinx installation.

Vivado/Vitis are proprietary and require a valid host license.  They are mounted
read-only into the relevant containers.

Example:

```bash
export DYNAMATIC_ROOT=$HOME/.local/src/dynamatic-v2.0.0
export OPENHLS_ROOT=$HOME/OpenHLS
export BAMBU_ROOT=$HOME/.local
export VIVADO_ROOT=/opt/Xilinx
docker compose build dynamatic bambu openhls vitis
```

## Run

The repository is mounted at `/workspace`.  The default benchmark path inside
containers is `/workspace/osshls_bench/kernels`; override `HLS_BENCHMARKS_ROOT`
when using a separate benchmark checkout.

```bash
docker compose run --rm dynamatic dynamatic --bench gcd --run-simulation
docker compose run --rm bambu bambu -- --benchmarks gcd --run-simulation
docker compose run --rm openhls openhls --bench gcd --run-simulation
docker compose run --rm vitis vitis --bench gcd --run-simulation
```

The LegUp flow remains launched by `hls_bench/scripts/legup_vivado_flow.sh`, which already invokes
Docker and uses the `legup14:latest` image.
