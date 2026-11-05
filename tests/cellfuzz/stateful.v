module stateful(input clk, rst, enable, input [1:0] address, input [7:0] data,
                output reg [7:0] q, output [7:0] read_data, output done);
  always @(posedge clk) q <= data + 1;
  reg [7:0] mem [0:3];
  always @(posedge clk) if (enable) mem[address] <= data;
  assign read_data = mem[address];
  (* fsm_encoding = "auto" *) reg [1:0] state;
  always @(posedge clk)
    if (rst) state <= 0;
    else case (state)
      0: state <= data[0] ? 1 : 0;
      1: state <= data[1] ? 2 : 1;
      2: state <= data[2] ? 0 : 2;
      default: state <= 0;
    endcase
  assign done = state == 2;
endmodule
