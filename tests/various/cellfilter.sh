set -e

python3 - <<'PY'
import os
from pathlib import Path
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix="cellfilter-") as work:
    work = Path(work)
    (work / "top.v").write_text('''
module top(input clk, rst, a, wen, input [1:0] addr, input [7:0] d,
           output reg q, output [7:0] m, output f, c);
  always @(posedge clk) q <= ~a;
  reg [7:0] mem [0:3];
  always @(posedge clk) if (wen) mem[addr] <= d;
  assign m = mem[addr];
  (* fsm_encoding = "auto" *) reg [1:0] state;
  always @(posedge clk)
    if (rst) state <= 0;
    else case (state)
      0: state <= a ? 1 : 0;
      1: state <= a ? 2 : 1;
      2: state <= a ? 0 : 2;
      default: state <= 0;
    endcase
  assign f = state == 2;
  assign c = ~a;
endmodule
''')
    prepare = f"read_verilog {work}/top.v; hierarchy -top top; proc; memory_collect; fsm -nomap; "

    def run(options, success=True):
        script = (prepare + f"write_rtlil {work}/before.il; " +
                  f"cellfilter {options} -dot {work}/graph.dot; " +
                  f"write_rtlil {work}/after.il")
        result = subprocess.run([os.environ["YOSYS"], "-Q", "-T", "-q", "-p", script],
                                capture_output=True, text=True, timeout=20)
        if not success:
            assert result.returncode != 0, options
            assert "node type" in result.stderr, result.stderr
            return
        assert result.returncode == 0, result.stderr
        assert (work / "before.il").read_bytes() == (work / "after.il").read_bytes(), options
        return (work / "graph.dot").read_text()

    types = ("register", "memory", "fsm")

    def counts(dot):
        return {kind: dot.count('\\n' + kind + '"];') for kind in types}

    original = run("")
    baseline = counts(original)
    assert all(baseline.values()), baseline
    assert run("-nodes all") == original
    assert run("-nodes register,memory,fsm") == original
    assert run("-nodes fsm,mem,register") == original
    for option, enabled in (
        ("register", {"register"}), ("memory", {"memory"}), ("mem", {"memory"}),
        ("fsm", {"fsm"}), ("register,memory", {"register", "memory"}), ("none", set()),
    ):
        for limit in ("", "-topk 1", "-topk 100"):
            dot = run(f"-nodes {option} {limit}")
            assert counts(dot) == {kind: baseline[kind] if kind in enabled else 0 for kind in types}, (option, limit)
    assert " -> " in run("-nodes none")
    assert run("-nodes register -nodes mem") == run("-nodes memory")
    for option in ("unknown", "register,,memory", "register,", ",fsm", "all,register"):
        run(f"-nodes {option}", success=False)

    # A path ending at an excluded state cell, or starting at its output, is
    # not retained. In particular, excluding a register cannot fold across it.
    (work / "barrier.v").write_text('''
module barrier(input clk, a, output y);
  reg q;
  always @(posedge clk) q <= ~a;
  assign y = ~q;
endmodule
''')
    script = (f"read_verilog {work}/barrier.v; proc; " +
              f"cellfilter -nodes none -topk 1 -dot {work}/barrier.dot")
    subprocess.run([os.environ["YOSYS"], "-Q", "-T", "-q", "-p", script], check=True, timeout=20)
    assert ' -> ' not in (work / "barrier.dot").read_text()

    result = subprocess.run([os.environ["YOSYS"], "-Q", "-T", "-p", "cellfilter -nodes"],
                            capture_output=True, text=True, timeout=20)
    assert result.returncode != 0 and "requires a node type list" in result.stderr
print("Cellfilter node selection and state-boundary tests passed.")
PY
