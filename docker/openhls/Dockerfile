FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang \
    cmake \
    git \
    lld \
    llvm \
    ninja-build \
    python3 \
    python3-pip \
    python3-venv \
    verilator \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
ENTRYPOINT ["python3", "hls-bench.py"]
CMD ["openhls", "--help"]
