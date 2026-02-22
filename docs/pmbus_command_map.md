# PMBus Command Map

This document maps every PMBus command used by the gateway and target to its wire format, data encoding, decode function, and validity bit.

---

## 1 Command Table

| # | Command | Code | Direction | Wire Format | Data Size | Encoding | Decode Function | Validity Bit |
|---|---------|------|-----------|-------------|-----------|----------|-----------------|-------------|
| 1 | `VOUT_MODE` | `0x20` | Read Byte | `[S][addr+W][0x20][Sr][addr+R][data][PEC?][P]` | 1 byte | Bits [7:5]=mode, [4:0]=signed exponent | `pmbus_vout_mode_exponent()` | — (cached) |
| 2 | `STATUS_WORD` | `0x79` | Read Word | `[S][addr+W][0x79][Sr][addr+R][lo][hi][PEC?][P]` | 2 bytes | Non-numeric bitmask | Direct (no decode) | `STATUS_VALID_WORD` (bit 0) |
| 3 | `STATUS_VOUT` | `0x7A` | Read Byte | `[S][addr+W][0x7A][Sr][addr+R][data][PEC?][P]` | 1 byte | Non-numeric bitmask | Direct (no decode) | `STATUS_VALID_VOUT` (bit 1) |
| 4 | `STATUS_IOUT` | `0x7B` | Read Byte | `[S][addr+W][0x7B][Sr][addr+R][data][PEC?][P]` | 1 byte | Non-numeric bitmask | Direct (no decode) | `STATUS_VALID_IOUT` (bit 2) |
| 5 | `STATUS_TEMPERATURE` | `0x7D` | Read Byte | `[S][addr+W][0x7D][Sr][addr+R][data][PEC?][P]` | 1 byte | Non-numeric bitmask | Direct (no decode) | `STATUS_VALID_TEMP` (bit 3) |
| 6 | `READ_VIN` | `0x88` | Read Word | `[S][addr+W][0x88][Sr][addr+R][lo][hi][PEC?][P]` | 2 bytes | Linear11 | `pmbus_linear11_to_milli()` | `TELEM_VALID_VIN` (bit 0) |
| 7 | `READ_IIN` | `0x89` | Read Word | `[S][addr+W][0x89][Sr][addr+R][lo][hi][PEC?][P]` | 2 bytes | Linear11 | `pmbus_linear11_to_milli()` | `TELEM_VALID_IIN` (bit 2) |
| 8 | `READ_VOUT` | `0x8B` | Read Word | `[S][addr+W][0x8B][Sr][addr+R][lo][hi][PEC?][P]` | 2 bytes | Linear16 | `pmbus_linear16_to_mv()` | `TELEM_VALID_VOUT` (bit 1) |
| 9 | `READ_IOUT` | `0x8C` | Read Word | `[S][addr+W][0x8C][Sr][addr+R][lo][hi][PEC?][P]` | 2 bytes | Linear11 | `pmbus_linear11_to_milli()` | `TELEM_VALID_IOUT` (bit 3) |
| 10 | `READ_TEMPERATURE_1` | `0x8D` | Read Word | `[S][addr+W][0x8D][Sr][addr+R][lo][hi][PEC?][P]` | 2 bytes | Linear11 | `pmbus_linear11_to_milli()` | `TELEM_VALID_TEMP1` (bit 4) |
| 11 | `READ_POUT` | `0x96` | Read Word | `[S][addr+W][0x96][Sr][addr+R][lo][hi][PEC?][P]` | 2 bytes | Linear11 | `pmbus_linear11_to_milli()` | `TELEM_VALID_POUT` (bit 5) |

---

## 2 Data Encoding Formats

### 2.1 Linear11 (signed 5+11)

Used by: `READ_VIN`, `READ_IIN`, `READ_IOUT`, `READ_TEMPERATURE_1`, `READ_POUT`.

```
Bit layout:  [15:11] = N (signed 5-bit exponent, –16..+15)
             [10:0]  = Y (signed 11-bit mantissa, –1024..+1023)

Real value = Y × 2^N
```

Gateway decode:
```c
int32_t milli = pmbus_linear11_to_milli(raw);   // → value × 1000
float   real  = pmbus_linear11_to_float(raw);   // → direct float
```

Target encode:
```c
uint16_t raw = mtb_pmbus_float_to_lin11(48.0f);  // → Linear11 word
```

### 2.2 Linear16 (unsigned 16-bit mantissa, separate exponent)

Used by: `READ_VOUT` only.

```
Bit layout:  [15:0] = Y (unsigned 16-bit mantissa)
Exponent N is read separately from VOUT_MODE command.

Real value = Y × 2^N
```

Gateway decode:
```c
// First, read exponent (once at startup):
int8_t exp = pmbus_vout_mode_exponent(vout_mode_byte);

// Then decode each VOUT reading:
uint32_t mV = pmbus_linear16_to_mv(raw, exp);   // → millivolts
```

Target encode:
```c
uint16_t raw = mtb_pmbus_float_to_lin16(12.0f, (int8_t)-12);
```

### 2.3 VOUT_MODE Register

```
Bit layout:  [7:5] = mode (must be 0b000 for linear format)
             [4:0] = signed 5-bit exponent (two's complement)

Typical value: 0xF4  →  mode=0b111(VID), exp=–12
               0x14  →  mode=0b000(linear), exp=–12
```

Extracted by `pmbus_vout_mode_exponent()`. The gateway caches this exponent per device and retries periodically on failure.

---

## 3 PEC (Packet Error Checking)

When `g_config.i2c.pec_enabled == true`, every transaction includes a CRC-8 byte:

| Transaction Type | PEC Input Bytes |
|-----------------|-----------------|
| Read Word | `[addr+W, cmd, addr+R, data_lo, data_hi]` → 5 bytes, PEC is 6th byte |
| Read Byte | `[addr+W, cmd, addr+R, data]` → 4 bytes, PEC is 5th byte |
| Send Byte | `[addr+W, cmd]` → 2 bytes, PEC is 3rd byte |

CRC polynomial: **0x07** (x⁸ + x² + x + 1), initial value 0x00.

Implementation: `pmbus_crc8()` in `pmbus_master.c`.

---

## 4 Error Handling

Each PMBus transaction goes through the retry loop in `pmbus_master.c`:

```
for attempt in 0..retries:
    result = i2c_master_transfer()
    if result == OK:
        if pec_enabled and pec_mismatch:
            metrics_inc_pmbus_pec_fail()
            continue
        return PMBUS_OK

    if result == NACK:
        metrics_inc_pmbus_nack()
    elif result == TIMEOUT:
        metrics_inc_pmbus_timeouts()
        if bus_recovery_enabled:
            pmbus_bus_recovery()  // 9× SCL toggles

    metrics_inc_pmbus_retries()

return last_error
```

On final failure, `metrics_inc_pmbus_reads_fail()` is called and the corresponding `valid_mask` bit remains clear in the telemetry/status record.

---

## 5 Validity Mask

### Telemetry (`telemetry_record_t.valid_mask`)

| Bit | Constant | Command |
|-----|----------|---------|
| 0 | `TELEM_VALID_VIN` | `READ_VIN` |
| 1 | `TELEM_VALID_VOUT` | `READ_VOUT` |
| 2 | `TELEM_VALID_IIN` | `READ_IIN` |
| 3 | `TELEM_VALID_IOUT` | `READ_IOUT` |
| 4 | `TELEM_VALID_TEMP1` | `READ_TEMPERATURE_1` |
| 5 | `TELEM_VALID_POUT` | `READ_POUT` |

`TELEM_VALID_ALL = 0x3F`

### Status (`status_record_t.valid_mask`)

| Bit | Constant | Command |
|-----|----------|---------|
| 0 | `STATUS_VALID_WORD` | `STATUS_WORD` |
| 1 | `STATUS_VALID_VOUT` | `STATUS_VOUT` |
| 2 | `STATUS_VALID_IOUT` | `STATUS_IOUT` |
| 3 | `STATUS_VALID_TEMP` | `STATUS_TEMPERATURE` |

`STATUS_VALID_ALL = 0x0F`

If a validity bit is clear, the corresponding value is omitted from the JSON payload.
