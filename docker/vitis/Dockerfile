FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

# Vitis HLS and Vivado are proprietary installations.  They are mounted at
# runtime; this image only supplies the portable runner environment.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    git \
    python3 \
    python3-pip \
    tcl \
    verilator \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
ENTRYPOINT ["python3", "hls-bench.py"]
CMD ["vitis", "--help"]
