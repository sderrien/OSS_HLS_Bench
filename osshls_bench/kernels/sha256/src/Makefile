KERNEL := sha256.c
TESTBENCH := main.c
OUTPUT_BASE := sha256
TOP_MODULE := sha256_block
RTL_RUNTIME_MAIN := $(abspath main.c)
RTL_RUNTIME_CFLAGS += -DSHA256_RTL_RUNTIME
RTL_RUNTIME_SUPPORT_SOURCES += $(abspath sha256_ref.cpp)

include ../../vibe-hls.mk
