FROM ubuntu:14.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
ENV LC_ALL=C.UTF-8

RUN apt-get update && apt-get install -y \
    build-essential \
    autoconf \
    automake \
    libtool \
    m4 \
    gcc \
    g++ \
    gcc-4.8 \
    gcc-4.8-plugin-dev \
    g++-4.8 \
    clang-3.5 \
    gcc-multilib \
    g++-multilib \
    libc6-dev-i386 \
    make \
    cmake \
    wget \
    git \
    tcl8.5-dev \
    liblpsolve55-dev \
    libgmp3-dev \
    libgmp-dev \
    libmysqlclient-dev \
    libxml2-dev \
    zlib1g-dev \
    python \
    pkg-config \
    texinfo \
    bison \
    flex \
    perl \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-4.8 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-4.8 100 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /legup

COPY legup-4.0 .

RUN mkdir -p cloog/install && cd cloog && \
    ./autogen.sh && cd isl && ./autogen.sh && cd .. && \
    ./configure --prefix=/legup/cloog/install && \
    make -j$(nproc) && make install

RUN cd llvm && \
    CC=gcc-4.8 CXX=g++-4.8 ./configure --with-cloog=/legup/cloog/install --with-isl=/legup/cloog/install && \
    make -j$(nproc)

RUN cd llvm-2.9_mips && \
    CC=gcc-4.8 CXX=g++-4.8 ./configure && make tools-only ONLY_TOOLS=llc -j$(nproc)

RUN make -C swtools/binutils -j1 VERBOSE=1 && \
    make -C llvm -j1 VERBOSE=1 && \
    make -C llvm-2.9_mips tools-only ONLY_TOOLS=llc -j1 VERBOSE=1 && \
    make -C dragonegg GCC=gcc-4.8 LLVM_CONFIG=../llvm/Release+Asserts/bin/llvm-config -j1 VERBOSE=1 && \
    make -C examples/lib/llvm -j1 VERBOSE=1 && \
    make -C swtools/lib-source -j1 VERBOSE=1 && \
    make -C tiger/processor -j1 VERBOSE=1 && \
    make -C tiger/linux_tools -j1 VERBOSE=1 && \
    make -C tiger/tool_source/profiling_tools -j1 VERBOSE=1

RUN echo 'Builder image ready!'


FROM ubuntu:14.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV LC_ALL=C.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
    clang-3.5 \
    gcc-4.8 \
    g++-4.8 \
    gcc-multilib \
    g++-multilib \
    libc6-dev-i386 \
    make \
    python \
    perl \
    tcl8.5 \
    liblpsolve55-dev \
    libgmp3-dev \
    libgmp-dev \
    libmysqlclient-dev \
    libxml2-dev \
    zlib1g-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /legup

COPY --from=builder /legup/examples /legup/examples
COPY --from=builder /legup/boards /legup/boards
COPY --from=builder /legup/swtools/binutils/bin /legup/swtools/binutils/bin
COPY --from=builder /legup/swtools/lib-source /legup/swtools/lib-source
COPY --from=builder /legup/tiger /legup/tiger
COPY --from=builder /legup/llvm/Release+Asserts /legup/llvm/Release+Asserts
COPY --from=builder /legup/llvm-2.9_mips/Release+Asserts /legup/llvm-2.9_mips/Release+Asserts
COPY --from=builder /legup/cloog/install /legup/cloog/install
COPY --from=builder /legup/dragonegg/dragonegg.so /legup/dragonegg/dragonegg.so
COPY --from=builder /legup/README /legup/README
COPY --from=builder /legup/LICENSE /legup/LICENSE

RUN echo 'Runtime image ready!'
