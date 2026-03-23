// Top module with various system tasks
// Tests rtl_trace ability to skip system task "noise"
`timescale 1ns/1ps

module system_tasks_top #(
    parameter DATA_WIDTH = 8,
    parameter FIFO_DEPTH = 16
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire                 enable,
    input  wire [DATA_WIDTH-1:0] data_in,
    output wire [DATA_WIDTH-1:0] data_out,
    output wire                 valid,
    output wire                 error_flag
);

    // Internal signals
    reg [DATA_WIDTH-1:0] data_reg;
    reg [DATA_WIDTH-1:0] fifo [FIFO_DEPTH-1:0];
    reg [$clog2(FIFO_DEPTH)-1:0] wr_ptr, rd_ptr;
    reg                  valid_reg;
    reg                  error_reg;
    
    // Counter for $display
    integer cycle_count;
    
    // File handle for $fwrite
    integer log_file;
    
    // Initial block with system tasks
    initial begin
        $display("[%0t] System Tasks Test Module Initialized", $time);
        $display("DATA_WIDTH = %0d, FIFO_DEPTH = %0d", DATA_WIDTH, FIFO_DEPTH);
        $write("Test started at %0t ns\n", $realtime);
        
        // $fwrite to a file (simulation only)
        // log_file = $fopen("test_log.txt", "w");
        // $fwrite(log_file, "Log file opened\n");
        
        cycle_count = 0;
    end
    
    // Monitor for signal changes (simulation only)
    initial begin
        // $monitor("At time %0t: enable=%b, data_in=%0d, valid=%b", 
        //          $time, enable, data_in, valid);
    end
    
    // Always block with system tasks
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            data_reg <= {DATA_WIDTH{1'b0}};
            wr_ptr <= {$clog2(FIFO_DEPTH){1'b0}};
            rd_ptr <= {$clog2(FIFO_DEPTH){1'b0}};
            valid_reg <= 1'b0;
            error_reg <= 1'b0;
            cycle_count <= 0;
            $display("[%0t] Reset asserted", $time);
        end else begin
            cycle_count <= cycle_count + 1;
            
            if (enable) begin
                // Data processing with $display for debug
                data_reg <= data_in;
                $display("[%0t] Cycle %0d: Processing data_in=%0d", $time, cycle_count, data_in);
                
                // FIFO write
                fifo[wr_ptr] <= data_in;
                wr_ptr <= wr_ptr + 1'b1;
                
                // FIFO read
                if (rd_ptr != wr_ptr) begin
                    rd_ptr <= rd_ptr + 1'b1;
                    valid_reg <= 1'b1;
                end else begin
                    valid_reg <= 1'b0;
                end
                
                // Error detection with $strobe
                if (data_in == 8'hFF) begin
                    error_reg <= 1'b1;
                    $strobe("[%0t] ERROR: Detected 0xFF pattern", $time);
                end else begin
                    error_reg <= 1'b0;
                end
            end
            
            // Periodic status using $write
            if (cycle_count == 100) begin
                $write("Status at cycle 100: wr_ptr=%0d, rd_ptr=%0d\n", wr_ptr, rd_ptr);
            end
        end
    end
    
    // Continuous assignment with system function
    assign data_out = fifo[rd_ptr];
    assign valid = valid_reg;
    assign error_flag = error_reg;
    
    // Final block
    final begin
        $display("[%0t] Simulation finished. Total cycles: %0d", $time, cycle_count);
        $display("Final state: wr_ptr=%0d, rd_ptr=%0d", wr_ptr, rd_ptr);
        // $fclose(log_file);
    end
    
    // Function with system task
    function automatic void print_status;
        input integer count;
        begin
            $display("Status: count=%0d", count);
        end
    endfunction
    
    // Task with system task
    task automatic log_event;
        input [7:0] event_code;
        begin
            $display("Event logged: code=%0h at time=%0t", event_code, $time);
        end
    endtask

endmodule
