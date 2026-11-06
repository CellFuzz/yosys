module query_nibble(input clk, input [2:0] address, input [7:0] data,
                    input [1:0] enable, output reg [7:0] q);
  reg [7:0] mem [5:7];
  initial begin
    mem[5] = 8'h11;
    mem[6] = 8'h22;
    mem[7] = 8'h33;
  end
  always @(posedge clk) begin
    if (enable[0]) mem[address][3:0] <= data[3:0];
    if (enable[1]) mem[address][7:4] <= data[7:4];
    q <= mem[address];
  end
endmodule
