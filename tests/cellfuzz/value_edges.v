// Standalone value/FF oracle fixture. Initial values deliberately differ from
// reset values so losing either init or a control connection is observable.
module value_edges (
    input clk, input en, input srst, input arst_n, input arst_p,
    input load, input latch_en, input [7:0] data, input [7:0] async_data,
    input [512:0] wide,
    output [0:0] w1, output [7:0] w8, output [31:0] w32,
    output [32:0] w33, output [63:0] w64, output [64:0] w65,
    output [255:0] w256, output [512:0] w513,
    output reg [7:0] q_pos, q_neg, q_en, q_sync, q_sync_en,
    output reg [7:0] q_async_n, q_async_p, q_load, q_latch
);
initial begin
    q_pos = 8'ha5; q_neg = 8'h5a; q_en = 8'h3c;
    q_sync = 8'h96; q_sync_en = 8'h69;
    q_async_n = 8'h12; q_async_p = 8'h34;
    q_load = 8'h56; q_latch = 8'h78;
end
assign w1 = ~wide[0:0];
assign w8 = ~wide[7:0];
assign w32 = ~wide[31:0];
assign w33 = ~wide[32:0];
assign w64 = ~wide[63:0];
assign w65 = ~wide[64:0];
assign w256 = ~wide[255:0];
assign w513 = ~wide;
always @(posedge clk) q_pos <= data;
always @(negedge clk) q_neg <= data;
always @(posedge clk) if (en) q_en <= data;
always @(posedge clk)
    if (srst) q_sync <= 8'he1;
    else if (en) q_sync <= data;
always @(posedge clk)
    if (en) begin
        if (srst) q_sync_en <= 8'hd2;
        else q_sync_en <= data;
    end
always @(posedge clk or negedge arst_n)
    if (!arst_n) q_async_n <= 8'hc3;
    else q_async_n <= data;
always @(posedge clk or posedge arst_p)
    if (arst_p) q_async_p <= 8'hb4;
    else q_async_p <= data;
always @(posedge clk or posedge load)
    if (load) q_load <= async_data;
    else q_load <= data;
always @* if (latch_en) q_latch <= data;
endmodule
