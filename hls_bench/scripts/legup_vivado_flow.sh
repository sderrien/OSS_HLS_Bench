#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: legup_vivado_flow.sh [options] path/to/file.c

Options:
  --top NAME           Vivado top module name (default: C file basename)
  --part PART          Vivado part number (required, or set VIVADO_PART)
  --period NS          Clock period for Vivado synthesis (default: 10.0)
  --clock-port NAME    Clock port name in generated Verilog (default: clk)
  --image NAME         LegUp Docker image (default: legup14:latest)
  --vivado CMD         Vivado command (default: vivado)
  --skip-vivado        Stop after LegUp synthesis
  --workdir DIR        Use DIR for generated files
    --cxx                Compile as C++ (force -x c++)
    --compile-define NAME
                        Pass -DNAME to the LegUp frontend (may be repeated)
  --preserve-function NAME
                       Preserve NAME as the HLS entry function (default: main)
EOF
}

top_name=""
vivado_part="${VIVADO_PART:-xc7a100tcsg324-1}"
clock_period="${CLOCK_PERIOD:-10.0}"
clock_port="clk"
docker_image="${LEGUP_IMAGE:-legup14:latest}"
vivado_cmd="${VIVADO:-vivado}"
skip_vivado=0
workdir=""
cxx_mode=0
preserve_function="main"
compile_defines=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --top)
      top_name="$2"
      shift 2
      ;;
    --part)
      vivado_part="$2"
      shift 2
      ;;
    --period)
      clock_period="$2"
      shift 2
      ;;
    --clock-port)
      clock_port="$2"
      shift 2
      ;;
    --image)
      docker_image="$2"
      shift 2
      ;;
    --vivado)
      vivado_cmd="$2"
      shift 2
      ;;
    --skip-vivado)
      skip_vivado=1
      shift
      ;;
    --workdir)
      workdir="$2"
      shift 2
      ;;
    --cxx)
      cxx_mode=1
      shift
      ;;
    --preserve-function)
      preserve_function="$2"
      shift 2
      ;;
    --compile-define)
      compile_defines+=("$2")
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      break
      ;;
  esac
done

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 1
fi

if [[ -z "$vivado_part" ]]; then
  echo "error: Vivado part is required (use --part or set VIVADO_PART)" >&2
  exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "error: docker is required" >&2
  exit 1
fi

if [[ "$skip_vivado" -eq 0 ]] && ! command -v "$vivado_cmd" >/dev/null 2>&1; then
  echo "error: Vivado command '$vivado_cmd' not found" >&2
  exit 1
fi

src_file=$(readlink -f "$1")
if [[ ! -f "$src_file" ]]; then
  echo "error: input file not found: $src_file" >&2
  exit 1
fi

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
examples_dir="$repo_root/legup-4.0/examples"
container_examples_dir="/legup/examples"
src_base=$(basename "$src_file")
stem=${src_base%.c}

if [[ -z "$top_name" ]]; then
  if [[ "$preserve_function" == "main" ]]; then
    top_name="top"
  else
    top_name="main"
  fi
fi

if [[ -z "$workdir" ]]; then
  workdir=$(mktemp -d "$examples_dir/.legup_${stem}.XXXXXX")
else
  rm -rf "$workdir"
  mkdir -p "$workdir"
fi
# Docker interprets a relative -v source as a named volume rather than a host
# directory.  Canonicalize both the mktemp default and caller-provided paths so
# batch runners can use repository-relative artifact directories without
# bypassing LegUp synthesis or staging files outside the requested work area.
workdir=$(readlink -f "$workdir")
cp "$src_file" "$workdir/$src_base"
src_dir=$(dirname "$src_file")
for hdr in "$src_dir"/*.h; do
  [ -f "$hdr" ] || continue
  cp "$hdr" "$workdir/$(basename "$hdr")"
done
if [[ -d "$src_dir/include" ]]; then
  mkdir -p "$workdir/include"
  cp -R "$src_dir/include/." "$workdir/include/"
fi
perl -0pi -e 's/__attribute__\s*\(\(annotate\("hls\.toplevel"\)\)\)/#pragma HLS top/g' "$workdir/$src_base"

cat > "$workdir/Makefile" <<EOF
NAME=$stem
LEVEL = $container_examples_dir
LEGUP_PRESERVE_FUNCTION = $preserve_function
EOF

if [[ "$vivado_part" == xc7a100* ]]; then
  python3 "$repo_root/hls_bench/generate_artix7_legup_config.py" --output "$workdir/config.tcl" --period "$clock_period"
  cat >> "$workdir/Makefile" <<'EOF'
LOCAL_CONFIG = -legup-config=config.tcl
EOF
fi

if [[ "$cxx_mode" -eq 1 ]]; then
  cat >> "$workdir/Makefile" <<'EOF'
FRONT_END = clang-3.5 -x c++
CFLAG += -I /usr/include/c++/14 -I /usr/include/x86_64-linux-gnu/c++/14
EOF
fi

cat >> "$workdir/Makefile" <<EOF
CFLAG += -I /work -I /work/include
EOF
for compile_define in "${compile_defines[@]}"; do
  cat >> "$workdir/Makefile" <<EOF
CFLAG += -D$compile_define
EOF
done
cat >> "$workdir/Makefile" <<'EOF'
include $(LEVEL)/Makefile.common
EOF

cat > "$workdir/run_vivado.tcl" <<EOF
set src "$workdir/$stem.v"
set outdir "$workdir/vivado"
set top "$top_name"
set part "$vivado_part"
set period "$clock_period"
set clock_port "$clock_port"
set fp_wrappers "$workdir/legup_xilinx_fp.v"

# These Xilinx IP latencies must match generate_artix7_legup_config.py. LegUp
# encodes each scheduled latency in the instantiated altfp_* module name.
set fp_project "\$outdir/fp_ip"
create_project legup_fp "\$fp_project" -part \$part -force
set fpo_ver 7.1
source "$repo_root/../dynamatic/tools/backend/synth-resources/addf_vitis_hls_single_precision_lat_8.tcl"
source "$repo_root/../dynamatic/tools/backend/synth-resources/subf_vitis_hls_single_precision_lat_8.tcl"
source "$repo_root/../dynamatic/tools/backend/synth-resources/mulf_vitis_hls_single_precision_lat_4.tcl"
source "$repo_root/../dynamatic/tools/backend/synth-resources/divf_vitis_hls_single_precision_lat_28.tcl"
create_ip -name floating_point -version \$fpo_ver -vendor xilinx.com -library ip -module_name legup_sitofp_7
set_property -dict [list CONFIG.operation_type Fixed_to_float CONFIG.has_aclken true] [get_ips legup_sitofp_7]
generate_target synthesis [get_ips legup_sitofp_7]

# Vivado 2026.1 may generate either Verilog or VHDL synthesis wrappers rather
# than the historical synth/*.v files. Traverse the generated project after
# target creation and retain only synthesizable wrappers, excluding duplicate
# simulation models. Returning an empty list is deliberately an error: the
# floating-point wrapper instantiates legup_sitofp_7, so omitting its generated
# HDL would invalidate the scheduled LegUp design rather than provide a usable
# fallback.
proc legup_find_generated_fp_hdl {directory} {
  set sources {}
  foreach entry [glob -nocomplain -directory \$directory *] {
    if {[file isdirectory \$entry]} {
      lappend sources {*}[legup_find_generated_fp_hdl \$entry]
    } elseif {[file tail [file dirname \$entry]] eq "synth" &&
              ([file extension \$entry] eq ".v" || [file extension \$entry] eq ".vhd")} {
      lappend sources \$entry
    }
  }
  return \$sources
}
set fp_hdl [legup_find_generated_fp_hdl \$fp_project]
if {[llength \$fp_hdl] == 0} {
  error "LegUp floating-point IP HDL was not generated below \$fp_project after generate_target synthesis for legup_sitofp_7"
}
foreach fp_src \$fp_hdl {
  if {[file extension \$fp_src] eq ".vhd"} {
    read_vhdl \$fp_src
  } else {
    read_verilog \$fp_src
  }
}

file mkdir \$outdir
foreach fp_src [list \$fp_wrappers] {
  if {[file exists \$fp_src]} {
    read_verilog \$fp_src
  }
}
read_verilog \$src
read_xdc [file join \$outdir clock.xdc]
synth_design -top \$top -part \$part
report_utilization -file [file join \$outdir util.rpt]
report_timing -delay_type max -max_paths 1 -nworst 1 -file [file join \$outdir timing.rpt]
write_checkpoint -force [file join \$outdir post_synth.dcp]
quit
EOF

cat > "$workdir/vivado.clock.xdc" <<EOF
create_clock -name $clock_port -period $clock_period [get_ports $clock_port]
EOF

cat > "$workdir/legup_xilinx_fp.v" <<'EOF'
// Adapts LegUp's clock-enable interface to rate-one Xilinx Floating Point IP.
// Each module's numeric suffix is its result latency in clock cycles.
module altfp_adder_8(input clk_en, input clock, input [31:0] dataa, input [31:0] datab, output [31:0] result);
  wire result_valid;
  addf_vitis_hls_single_precision_lat_8 impl(.aclk(clock), .aclken(1'b1), .s_axis_a_tvalid(clk_en), .s_axis_a_tdata(dataa), .s_axis_b_tvalid(clk_en), .s_axis_b_tdata(datab), .m_axis_result_tvalid(result_valid), .m_axis_result_tdata(result));
endmodule

module altfp_subtractor_8(input clk_en, input clock, input [31:0] dataa, input [31:0] datab, output [31:0] result);
  wire result_valid;
  subf_vitis_hls_single_precision_lat_8 impl(.aclk(clock), .aclken(1'b1), .s_axis_a_tvalid(clk_en), .s_axis_a_tdata(dataa), .s_axis_b_tvalid(clk_en), .s_axis_b_tdata(datab), .m_axis_result_tvalid(result_valid), .m_axis_result_tdata(result));
endmodule

module altfp_multiplier_4(input clk_en, input clock, input [31:0] dataa, input [31:0] datab, output [31:0] result);
  wire result_valid;
  mulf_vitis_hls_single_precision_lat_4 impl(.aclk(clock), .aclken(1'b1), .s_axis_a_tvalid(clk_en), .s_axis_a_tdata(dataa), .s_axis_b_tvalid(clk_en), .s_axis_b_tdata(datab), .m_axis_result_tvalid(result_valid), .m_axis_result_tdata(result));
endmodule

module altfp_divider_33(input clk_en, input clock, input [31:0] dataa, input [31:0] datab, output [31:0] result);
  wire result_valid;
  wire [31:0] ip_result;
  reg [31:0] delay0, delay1, delay2, delay3, delay4;
  divf_vitis_hls_single_precision_lat_28 impl(.aclk(clock), .aclken(1'b1), .s_axis_a_tvalid(clk_en), .s_axis_a_tdata(dataa), .s_axis_b_tvalid(clk_en), .s_axis_b_tdata(datab), .m_axis_result_tvalid(result_valid), .m_axis_result_tdata(ip_result));
  always @(posedge clock) begin
    delay0 <= ip_result;
    delay1 <= delay0;
    delay2 <= delay1;
    delay3 <= delay2;
    delay4 <= delay3;
  end
  assign result = delay4;
endmodule

module altfp_sitofp32_7(input clk_en, input clock, input [31:0] dataa, output [31:0] result);
  wire result_valid;
  wire input_ready;
  legup_sitofp_7 impl(.aclk(clock), .aclken(1'b1), .s_axis_a_tvalid(clk_en), .s_axis_a_tready(input_ready), .s_axis_a_tdata(dataa), .m_axis_result_tvalid(result_valid), .m_axis_result_tready(1'b1), .m_axis_result_tdata(result));
endmodule

module lpm_add_sub #(parameter lpm_width = 1, parameter lpm_direction = "UNUSED", parameter lpm_pipeline = 0, parameter lpm_representation = "UNSIGNED", parameter lpm_type = "lpm_add_sub", parameter lpm_hint = "UNUSED")
  (input [lpm_width-1:0] dataa, input [lpm_width-1:0] datab, input cin, input add_sub, input clock, input clken, input aclr, output [lpm_width-1:0] result, output cout, output overflow);
  wire do_sub = (lpm_direction == "SUB") || (lpm_direction == "UNUSED" && add_sub == 1'b0);
  wire [lpm_width:0] full = do_sub ? ({1'b0, dataa} - {1'b0, datab} - {{lpm_width{1'b0}}, cin}) : ({1'b0, dataa} + {1'b0, datab} + {{lpm_width{1'b0}}, cin});
  assign result = full[lpm_width-1:0];
  assign cout = full[lpm_width];
  assign overflow = 1'b0;
endmodule

module lpm_mult #(parameter lpm_widtha = 1, parameter lpm_widthb = 1, parameter lpm_widthp = lpm_widtha + lpm_widthb, parameter lpm_widths = 1, parameter lpm_pipeline = 0, parameter lpm_representation = "UNSIGNED", parameter lpm_type = "lpm_mult", parameter lpm_hint = "UNUSED")
  (input [lpm_widtha-1:0] dataa, input [lpm_widthb-1:0] datab, input [lpm_widths-1:0] sum, input clock, input clken, input aclr, output [lpm_widthp-1:0] result);
  assign result = dataa * datab;
endmodule

module lpm_compare #(parameter lpm_width = 1, parameter lpm_representation = "UNSIGNED", parameter lpm_type = "lpm_compare", parameter lpm_hint = "UNUSED")
  (input [lpm_width-1:0] dataa, input [lpm_width-1:0] datab, input clock, input clken, input aclr, output alb, output aleb, output aeb, output aneb, output ageb, output agb);
  assign alb = dataa < datab;
  assign aleb = dataa <= datab;
  assign aeb = dataa == datab;
  assign aneb = dataa != datab;
  assign ageb = dataa >= datab;
  assign agb = dataa > datab;
endmodule

module lpm_mux #(parameter lpm_width = 1, parameter lpm_size = 1, parameter lpm_widths = 1, parameter lpm_pipeline = 0, parameter lpm_type = "lpm_mux", parameter lpm_hint = "UNUSED")
  (input [(lpm_width*lpm_size)-1:0] data, input [lpm_widths-1:0] sel, input clock, input clken, input aclr, output [lpm_width-1:0] result);
  assign result = data[sel * lpm_width +: lpm_width];
endmodule

module __legup_label #(parameter tag_offset = 0)
  (input clk, input clk2x, input clk1x_follower, input reset, input start, output finish, input [31:0] arg_0);
  assign finish = start;
endmodule

module _Z13__legup_labelPc #(parameter tag_offset = 0)
  (input clk, input clk2x, input clk1x_follower, input reset, input start, output finish, input [31:0] arg_0);
  assign finish = start;
endmodule
EOF

rel_workdir=${workdir#"$repo_root"/}
container_workdir="/legup/$rel_workdir"

echo "[LegUp] synthesizing $src_file in $workdir"
docker run --rm \
  -v "$examples_dir/Makefile.common:$container_examples_dir/Makefile.common:ro" \
  -v "$workdir:/work" \
  -w /work \
  "$docker_image" \
  make -j1 VERBOSE=1

vfile="$workdir/$stem.v"
if [[ ! -f "$vfile" ]]; then
  echo "error: LegUp did not produce $vfile" >&2
  exit 1
fi

if [[ "$skip_vivado" -eq 0 ]]; then
  mkdir -p "$workdir/vivado"
  cp "$workdir/vivado.clock.xdc" "$workdir/vivado/clock.xdc"
  echo "[Vivado] running post-synthesis reports"
  # Vivado 2026.1 still loads libncurses.so.5, which Ubuntu 24 no longer ships.
  # Derive the bundled compatibility directory from the selected Vivado binary
  # and expose it only to this subprocess so the surrounding tool flow is not
  # affected by Xilinx's legacy library dependencies.
  vivado_bin_path=$(readlink -f "$(command -v "$vivado_cmd")")
  vivado_root=$(dirname "$(dirname "$vivado_bin_path")")
  vivado_ncurses_dir="$vivado_root/lib/lnx64.o/Ubuntu/24"
  if [[ -f "$vivado_ncurses_dir/libncurses.so.5" ]]; then
    LD_LIBRARY_PATH="$vivado_ncurses_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      "$vivado_cmd" -mode batch -source "$workdir/run_vivado.tcl" >"$workdir/vivado/vivado.log" 2>&1
  else
    "$vivado_cmd" -mode batch -source "$workdir/run_vivado.tcl" >"$workdir/vivado/vivado.log" 2>&1
  fi
fi

util_rpt="$workdir/vivado/util.rpt"
timing_rpt="$workdir/vivado/timing.rpt"

extract_util() {
  local pattern="$1"
  local line
  line=$(grep -m1 -E "$pattern" "$util_rpt" || true)
  if [[ -z "$line" ]]; then
    return 1
  fi
  awk -F'|' '{
    for (i = 1; i <= NF; i++) {
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", $i)
      gsub(/,/, "", $i)
    }
    if (NF >= 3) {
      print $3
    } else if (NF >= 2) {
      print $2
    }
  }' <<< "$line"
}

tclk="n/a"
if [[ -f "$timing_rpt" ]]; then
  tclk=$(grep -m1 'Data Path Delay:' "$timing_rpt" | sed -E 's/.*Data Path Delay:[[:space:]]*([0-9.]+)ns.*/\1/' || true)
fi

lut="n/a"
ff="n/a"
bram="n/a"
dsp="n/a"
if [[ -f "$util_rpt" ]]; then
  lut=$(extract_util 'CLB LUTs|Slice LUTs|LUTs' || true)
  ff=$(extract_util 'CLB Registers|Slice Registers|Registers' || true)
  bram=$(extract_util 'Block RAM Tile|BRAM|RAMB' || true)
  dsp=$(extract_util 'DSPs' || true)
fi

echo "Result"
echo "  C file: $src_file"
echo "  Top: $top_name"
echo "  Verilog: $vfile"
echo "  LUTs: ${lut:-n/a}"
echo "  FFs: ${ff:-n/a}"
echo "  BRAM: ${bram:-n/a}"
echo "  DSPs: ${dsp:-n/a}"
echo "  Estimated Tclk (ns): ${tclk:-n/a}"
echo "  Vivado reports: $workdir/vivado"
