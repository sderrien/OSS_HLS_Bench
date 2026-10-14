FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

RUN apt-get update && apt-get install -y --no-install-recommends \
    bison \
    build-essential \
    cmake \
    flex \
    g++ \
    gcc \
    git \
    libboost-all-dev \
    libfl-dev \
    libgmp-dev \
    libmpc-dev \
    libmpfr-dev \
    libxml2-dev \
    make \
    python3 \
    python3-pip \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
ENTRYPOINT ["python3", "hls-bench.py"]
CMD ["bambu", "--help"]
