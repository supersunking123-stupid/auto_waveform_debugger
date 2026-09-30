# Merged root range display (E4c experiment)

This is a USER DECISION prototype. It changes selected trace output. It does not imply retention in the default implementation.

A newly compiled endpoint that combines at least two exact ranges carries reserved bit 0x01. Logical-axis metadata independently uses bit 0x02; a merged logical row can carry both bits (0x03). The database record size and v5 format remain unchanged. Compile semantics epoch 4 forces an incremental rebuild of epoch-3 databases.

For an ordinary scalar merged root endpoint [7:0], a query x[2] prints [2]. A query x[6:3] prints [6:3]. Ascending endpoint orientation is retained when displaying a range. Text and JSON use the same narrowed output copy.

The original endpoint key controls output dedup before clipping. All traversal and stop decisions use the original endpoint. Cached session records retain their full ranges. Repeated serve queries do not accumulate clipping.

Only endpoints stored for the root being walked and whose path identifies that root can narrow. Endpoints reached through ports or deeper cone traversal keep their own full bitmaps. Port projections stored on the root can still use the connected signal's coordinates. They retain their full display. Fallback driver records come from another signal's load list and retain their full displays too, even when presented while walking the requested root.

Clipping is skipped for ports, approximate or symbolic selectors, logical-axis metadata, multiple brackets, and struct-member roots. These require a precise coordinate policy beyond this scalar display change. The existing one-dimensional selector normalization and overlap filter remain unchanged.

Old database records without 0x01 are read without inferred merged provenance. Their output stays unchanged. New flags are internal metadata; no JSON fields, command options, or MCP contracts are added.
