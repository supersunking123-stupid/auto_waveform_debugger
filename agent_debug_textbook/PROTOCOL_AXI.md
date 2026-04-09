# AXI Protocol Reference for RTL Debugging

This document records AXI-specific conventions, signal semantics, and common mis-attributions that arise during waveform debugging. Consult it when debugging AXI interfaces or when an AXI VIP/protocol checker fires an error.

For general debug methodology, see `rtl_debug_guide.md`. For normalization rules (radix, unit, convention differences between sources), see Rule 9.

This document assumes the protocol error came from an **EDA-vendor AXI VIP monitor/checker**. That VIP checker is golden. A home-grown memory model, BFM, wrapper, or scoreboard attached to the interface is **not** automatically golden.

---

## 1. ARLEN / AWLEN semantics

AXI burst length is encoded **0-based** on the waveform:

| Waveform value | Actual burst length | Number of beats |
|---|---|---|
| `ARLEN = 0x0` | 1 | 1 |
| `ARLEN = 0x1` | 2 | 2 |
| `ARLEN = 0x3` | 4 | 4 |
| `ARLEN = 0xF` | 16 | 16 |

**Formula:** beats = ARLEN + 1

Many VIPs report burst length **1-based** in their transaction logs:

| VIP log field | Waveform signal | Relationship |
|---|---|---|
| `LENGTH('d1)` | `ARLEN = 0x0` | VIP 1-based = waveform 0-based + 1 |
| `LENGTH('d4)` | `ARLEN = 0x3` | |

**Rule:** When searching for a VIP-reported transaction in the waveform, subtract 1 from the VIP's `LENGTH` field before querying `ARLEN`. Example: VIP says `LENGTH('d1)` → search for `ARLEN == 0` on the waveform.

---

## 2. ARID / AWID reuse — multiple outstanding transactions with the same ID

**AXI3 and AXI4 both allow multiple outstanding transactions with the same ID.** This is legal and intentional. The protocol guarantees that responses for the same ID return in the order the requests were issued — but having two or more in-flight requests with ARID=0x08 is not a protocol violation.

**Do not flag same-ID reuse as a bug.** If the DUT issues two AR requests with ARID=0x08 in sequence, this is legal AXI behavior.

What the ordering guarantee means in practice:
- If the DUT issues AR1(ID=8) then AR2(ID=8), the slave must return the R response for AR1 before the R response for AR2.
- The slave is responsible for tracking the order. Testbench memory models typically use a FIFO to store per-transaction metadata (burst length, address) keyed by issue order, not by ID.

**Common mis-attribution:** observing two outstanding transactions with the same ID and concluding "the DUT violated AXI protocol." This conclusion is incorrect. Do not pursue it.

---

## 3. RLAST / WLAST semantics

- `RLAST` must be asserted on the **last beat** of a read burst and deasserted on all earlier beats.
- For a single-beat burst (`ARLEN=0`), `RLAST` must be 1 on beat 0 (the only beat).
- The slave (memory model or DUT, depending on direction) is responsible for asserting RLAST at the right beat. It typically tracks remaining beats using the burst length stored in a metadata FIFO.

**When a VIP fires `rlast_asserted_for_last_read_data_beat`:** the slave sent the wrong number of beats — either too many (RLAST=0 on what should be the last beat) or too few (RLAST=1 before the last beat). Trace backward from RLAST to find what burst-length value the slave used and where that value came from.

---

## 4. AXI signal name mapping (waveform vs. VIP log vs. spec)

VIP transaction logs use protocol-level field names; waveforms use design-specific signal names. They refer to the same physical signals.

| AXI spec name | Typical waveform signal | VIP log field |
|---|---|---|
| `ARLEN[3:0]` | `<if_name>_arlen[3:0]` | `LENGTH` (1-based) |
| `ARID[n:0]` | `<if_name>_arid[n:0]` | `ID` |
| `ARADDR` | `<if_name>_araddr` | `ADDR` |
| `ARVALID` | `<if_name>_arvalid` | (handshake, not usually logged) |
| `ARREADY` | `<if_name>_arready` | (handshake) |
| `RLAST` | `<if_name>_rlast` | `RLAST` |
| `RVALID` | `<if_name>_rvalid` | (handshake) |
| `RID` | `<if_name>_rid` | `ID` on R channel |

`<if_name>` varies by design (e.g., `axi_slave1`, `nvdla2cvsram`). Use `list_signals` scoped to the interface instance to find the exact signal names.

---

## 5. Normalization checklist

Before comparing a value from the VIP log against a waveform signal:

1. **Burst length:** subtract 1 from VIP `LENGTH` before comparing to `ARLEN`/`AWLEN`.
2. **ID:** convert both to the same radix (the VIP may log decimal, the waveform shows hex).
3. **Address:** confirm both are byte addresses (some designs use word-aligned addresses on the waveform).
4. **Timestamp:** convert VIP nanoseconds to waveform precision units (multiply by 1000 for 1ns/1ps timescale).

---

## 6. Common mis-attributions to avoid

| Observation | Wrong conclusion | Correct interpretation |
|---|---|---|
| DUT issues two AR requests with the same ARID | "DUT violated AXI protocol — same ID reuse is illegal" | Legal — AXI allows same-ID reuse with in-order response guarantee |
| VIP reports `LENGTH('d1)` but waveform shows `ARLEN=0x0` | "Burst length mismatch between VIP and DUT" | Same value — VIP is 1-based, waveform is 0-based |
| RLAST=0 on the last beat of a 1-beat burst | "DUT generated the wrong RLAST" | May be testbench memory model using wrong burst-length metadata — check who drives RLAST with `rtl_trace trace --mode drivers` |
| VIP fires a read data channel error | "Bug is in DUT's R-channel logic" | The wrong producer may be DUT RTL or home-grown testbench RTL; trace the immediate driver before attributing the source |

---

## 7. General start-point rule — begin from the flagged signal

This is **not AXI-specific**. It applies to any debug session where a UVM error,
assertion message, scoreboard report, or monitor log identifies a specific
signal as wrong.

**Rule:** if the error message flags a signal explicitly, that signal is the best
starting point for debugging. Do not start from a nearby internal signal chosen
by intuition. Start from the signal the checker or error message actually names,
then trace its immediate driver.

General workflow:

1. Extract the flagged signal from the error message.
2. Use `rtl_trace trace --mode drivers --signal <flagged_signal>` to find its immediate driver.
3. If the driver is **DUT RTL**: proceed into the DUT.
4. If the driver is **testbench RTL** (e.g., a memory model, BFM, wrapper, or monitor logic): the investigation shifts to that testbench logic.
5. Do not question an EDA-vendor VIP checker that flagged the signal. Instead, identify which RTL block produced the wrong value that reached it.

AXI-specific application:

- If an AXI VIP flags `RLAST`, start from `RLAST`.
- If an AXI VIP flags `RVALID`, start from `RVALID`.
- If a log points to `ARLEN`, start from `ARLEN`.

The protocol-specific material in this document helps you interpret those AXI
signals correctly once you have chosen the flagged signal as the starting point.
