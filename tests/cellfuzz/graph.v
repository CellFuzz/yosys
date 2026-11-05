module graph(input clk, input [7:0] a, b, output [7:0] y, z);
  wire [7:0] first = ~a;
  wire [7:0] second = first ^ b;
  reg [7:0] q;
  always @(posedge clk) q <= second;
  assign y = ~(q & b);
  assign z = (a | b) + (a ^ b);
endmodule
