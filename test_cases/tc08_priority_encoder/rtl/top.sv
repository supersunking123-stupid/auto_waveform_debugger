// Top module with priority encoder
// Tests priority if-else chains and case/casex
`timescale 1ns/1ps

module priority_encoder_top #(
    parameter NUM_INPUTS = 8,
    parameter LOG_NUM    = 3  // log2(NUM_INPUTS)
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire [NUM_INPUTS-1:0] req,
    input  wire                 mode,  // 0: if-else priority, 1: case priority
    output wire [LOG_NUM-1:0]    grant_idx,
    output wire [NUM_INPUTS-1:0] grant_onehot,
    output wire                 valid
);

    reg [LOG_NUM-1:0]    grant_idx_reg;
    reg [NUM_INPUTS-1:0] grant_onehot_reg;
    reg                  valid_reg;

    // Priority encoding using if-else chain
    reg [LOG_NUM-1:0]    grant_idx_if;
    reg [NUM_INPUTS-1:0] grant_onehot_if;
    reg                  valid_if;

    always @(*) begin
        grant_idx_if    = {LOG_NUM{1'b0}};
        grant_onehot_if = {NUM_INPUTS{1'b0}};
        valid_if        = 1'b0;
        
        // Priority: highest index has highest priority
        if      (req[NUM_INPUTS-1]) begin
            grant_idx_if    = LOG_NUM'(NUM_INPUTS-1);
            grant_onehot_if = {NUM_INPUTS{1'b0}} | (1'b1 << (NUM_INPUTS-1));
            valid_if        = 1'b1;
        end else if (req[NUM_INPUTS-2]) begin
            grant_idx_if    = LOG_NUM'(NUM_INPUTS-2);
            grant_onehot_if = {NUM_INPUTS{1'b0}} | (1'b1 << (NUM_INPUTS-2));
            valid_if        = 1'b1;
        end else if (req[NUM_INPUTS-3]) begin
            grant_idx_if    = LOG_NUM'(NUM_INPUTS-3);
            grant_onehot_if = {NUM_INPUTS{1'b0}} | (1'b1 << (NUM_INPUTS-3));
            valid_if        = 1'b1;
        end else if (req[NUM_INPUTS-4]) begin
            grant_idx_if    = LOG_NUM'(NUM_INPUTS-4);
            grant_onehot_if = {NUM_INPUTS{1'b0}} | (1'b1 << (NUM_INPUTS-4));
            valid_if        = 1'b1;
        end else if (req[NUM_INPUTS-5]) begin
            grant_idx_if    = LOG_NUM'(NUM_INPUTS-5);
            grant_onehot_if = {NUM_INPUTS{1'b0}} | (1'b1 << (NUM_INPUTS-5));
            valid_if        = 1'b1;
        end else if (req[NUM_INPUTS-6]) begin
            grant_idx_if    = LOG_NUM'(NUM_INPUTS-6);
            grant_onehot_if = {NUM_INPUTS{1'b0}} | (1'b1 << (NUM_INPUTS-6));
            valid_if        = 1'b1;
        end else if (req[NUM_INPUTS-7]) begin
            grant_idx_if    = LOG_NUM'(NUM_INPUTS-7);
            grant_onehot_if = {NUM_INPUTS{1'b0}} | (1'b1 << (NUM_INPUTS-7));
            valid_if        = 1'b1;
        end else if (req[0]) begin
            grant_idx_if    = LOG_NUM'(0);
            grant_onehot_if = {NUM_INPUTS{1'b0}} | 1'b1;
            valid_if        = 1'b1;
        end
    end

    // Priority encoding using case (synthesizable priority)
    reg [LOG_NUM-1:0]    grant_idx_case;
    reg [NUM_INPUTS-1:0] grant_onehot_case;
    reg                  valid_case;

    always @(*) begin
        grant_idx_case    = {LOG_NUM{1'b0}};
        grant_onehot_case = {NUM_INPUTS{1'b0}};
        valid_case        = 1'b0;
        
        casex (req)
            8'b1xxxxxxx: begin grant_idx_case = 7; grant_onehot_case = 8'b10000000; valid_case = 1; end
            8'b01xxxxxx: begin grant_idx_case = 6; grant_onehot_case = 8'b01000000; valid_case = 1; end
            8'b001xxxxx: begin grant_idx_case = 5; grant_onehot_case = 8'b00100000; valid_case = 1; end
            8'b0001xxxx: begin grant_idx_case = 4; grant_onehot_case = 8'b00010000; valid_case = 1; end
            8'b00001xxx: begin grant_idx_case = 3; grant_onehot_case = 8'b00001000; valid_case = 1; end
            8'b000001xx: begin grant_idx_case = 2; grant_onehot_case = 8'b00000100; valid_case = 1; end
            8'b0000001x: begin grant_idx_case = 1; grant_onehot_case = 8'b00000010; valid_case = 1; end
            8'b00000001: begin grant_idx_case = 0; grant_onehot_case = 8'b00000001; valid_case = 1; end
            default:     begin grant_idx_case = 0; grant_onehot_case = 8'b00000000; valid_case = 0; end
        endcase
    end

    // Output selection based on mode
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            grant_idx_reg    <= {LOG_NUM{1'b0}};
            grant_onehot_reg <= {NUM_INPUTS{1'b0}};
            valid_reg        <= 1'b0;
        end else begin
            if (mode) begin
                grant_idx_reg    <= grant_idx_case;
                grant_onehot_reg <= grant_onehot_case;
                valid_reg        <= valid_case;
            end else begin
                grant_idx_reg    <= grant_idx_if;
                grant_onehot_reg <= grant_onehot_if;
                valid_reg        <= valid_if;
            end
        end
    end

    assign grant_idx    = grant_idx_reg;
    assign grant_onehot = grant_onehot_reg;
    assign valid        = valid_reg;

endmodule
