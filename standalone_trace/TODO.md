# Future Optimizations

## Zero-Copy String View Trace Cache
Currently, the `TraceCompileCache` bounds the memory spike by flushing the `body_trace_indexes` when memory inflates over 200 module definitions. This limits `SaveGraphDb` memory to ~4.4GB but costs ~15 seconds of wall time due to AST Cache Misses.

**Goal:** Achieve both the 34s speed and 4.4GB memory simultaneously.

**Implementation Steps:**
1. Refactor `SymbolRefList` from `std::vector<std::string>` to `std::vector<std::string_view>`.
2. Update `CollectSignalsFromExpr` to return string views that point natively into Slang's existing `name` allocations.
3. Cascade the `std::string_view` adjustments through `ExprTraceResult`, `TraceResult`, and all `TraceCompileCache` layers.
4. Remove the `body_trace_indexes.size() > 200` cache flush check entirely, allowing the cache to grow unbounded but cost zero extra memory beyond vector capacities.

This fixes the memory penalty natively without discarding compiled structures, eliminating the speed regression!
