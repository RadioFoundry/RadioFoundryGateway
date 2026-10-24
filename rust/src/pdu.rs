// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! DMR layer 2 PDUs (rdof/pdu.h): full LC, CSBK, data headers and data blocks. A port of common/src/pdu.c; the FEC and CRCs are the C's (rdof/fec.h), called through ffi.
//!
//! The data block functions are the RDOF_FEATURE_DATA_BLOCKS build's (the host tests have every feature on).

use core::ffi::c_int;

use crate::bits::{get_field, slice_mut};
use crate::ffi::{self, RDOF_BURST_BYTES, RDOF_DT_RATE_1, RDOF_DT_RATE_12, RDOF_DT_RATE_34, RDOF_DT_VOICE_LC_HEADER};

type Burst = [u8; RDOF_BURST_BYTES];

const RDOF_FLCO_GROUP: c_int = 0x00;
const RDOF_FLCO_PRIVATE: c_int = 0x03;
const RDOF_FLCO_TA_HEADER: c_int = 0x04;
const RDOF_FLCO_TA_BLOCK1: c_int = 0x05;
const RDOF_FLCO_TA_BLOCK3: c_int = 0x07;
const RDOF_FLCO_GPS_INFO: c_int = 0x08;

const RDOF_DPF_UDT: c_int = 0;
const RDOF_DPF_RESPONSE: c_int = 1;
const RDOF_DPF_UNCONFIRMED: c_int = 2;
const RDOF_DPF_CONFIRMED: c_int = 3;
const RDOF_DPF_SHORT_DATA_DEFINED: c_int = 13;
const RDOF_DPF_SHORT_DATA_RAW: c_int = 14;
const RDOF_DPF_PROPRIETARY: c_int = 15;

const RDOF_CSBK_PREAMBLE: c_int = 0x3D;

/// "Not present" for the optional ids and fields.
const RDOF_NONE: c_int = -1;

/// rdof_full_lc_t, field for field.
#[repr(C)]
pub struct RdofFullLc {
    pub pf: bool,
    pub flco: c_int,
    pub fid: c_int,
    pub raw: [u8; 9],
    pub src_id: i32,
    pub dst_id: i32,
    pub group: bool,
    pub service_options: c_int,
    pub has_gps: bool,
    pub lat: f64,
    pub lon: f64,
    pub position_error: c_int,
    pub ta_format: c_int,
    pub ta_length: c_int,
    pub ta_bits: [u8; 56],
    pub ta_nbits: c_int,
}

/// rdof_csbk_t, field for field.
#[repr(C)]
#[derive(Default)]
pub struct RdofCsbk {
    pub opcode: c_int,
    pub fid: c_int,
    pub last_block: bool,
    pub pf: bool,
    pub raw: [u8; 12],
    pub crc_ok: bool,
    pub src_id: i32,
    pub dst_id: i32,
    pub group: bool,
    pub data_follows: bool,
    pub blocks_to_follow: c_int,
}

/// rdof_data_header_t, field for field.
#[repr(C)]
pub struct RdofDataHeader {
    pub dpf: c_int,
    pub sap: c_int,
    pub group: bool,
    pub response_requested: bool,
    pub dst_id: u32,
    pub src_id: u32,
    pub raw: [u8; 12],
    pub crc_ok: bool,
    pub blocks: c_int,
    pub pad_octets: c_int,
    pub full_message: bool,
    pub fsn: c_int,
    pub ns: c_int,
    pub udt_format: c_int,
    pub pad_nibbles: c_int,
    pub udt_opcode: c_int,
    pub dd_format: c_int,
    pub bit_padding: c_int,
    pub response_class: c_int,
    pub response_type: c_int,
    pub response_status: c_int,
    pub mfid: c_int,
}

/// rdof_data_block_t, field for field.
#[repr(C)]
pub struct RdofDataBlock {
    pub data_type: c_int,
    pub payload: [u8; 24],
    pub len: c_int,
    pub fec_ok: bool,
    pub errors: c_int,
}

/// All zeros, padding included, as the C's memset. Only for the structs above: zero is a valid value of each.
fn zero<T>(o: &mut T) {
    unsafe { core::ptr::write_bytes(o as *mut T, 0, 1) };
}

/// `width` bits of `bytes` from `start` as an int.
fn field(bytes: &[u8], start: usize, width: usize) -> c_int {
    get_field(bytes, start, width) as c_int
}

fn signed_field(v: u64, width: u32) -> i64 {
    if v & (1 << (width - 1)) != 0 {
        v as i64 - (1i64 << width)
    } else {
        v as i64
    }
}

/// A 12-byte payload from a data / control burst's BPTC(196,96), and whether it decoded.
fn bptc(burst: &Burst) -> ([u8; 12], bool) {
    let mut d = [0u8; 12];
    let ok = unsafe { ffi::rdof_bptc196_decode(burst.as_ptr(), d.as_mut_ptr()) };
    (d, ok)
}

#[no_mangle]
pub extern "C" fn rdof_parse_full_lc(lc9: &[u8; 9], o: &mut RdofFullLc) {
    zero(o);
    o.raw = *lc9;
    o.pf = get_field(lc9, 0, 1) != 0;
    o.flco = field(lc9, 2, 6);
    o.fid = field(lc9, 8, 8);
    o.src_id = RDOF_NONE;
    o.dst_id = RDOF_NONE;
    o.ta_format = RDOF_NONE;
    o.ta_length = RDOF_NONE;
    o.position_error = RDOF_NONE;
    if o.flco == RDOF_FLCO_GROUP || o.flco == RDOF_FLCO_PRIVATE {
        o.service_options = lc9[2].into();
        o.dst_id = field(lc9, 24, 24);
        o.src_id = field(lc9, 48, 24);
        o.group = o.flco == RDOF_FLCO_GROUP;
    } else if o.fid == 0 && o.flco == RDOF_FLCO_GPS_INFO {
        o.has_gps = true;
        o.position_error = field(lc9, 20, 3);
        o.lon = signed_field(get_field(lc9, 23, 25), 25) as f64 * 360.0 / (1 << 25) as f64;
        o.lat = signed_field(get_field(lc9, 48, 24), 24) as f64 * 180.0 / (1 << 24) as f64;
    } else if o.fid == 0 && o.flco == RDOF_FLCO_TA_HEADER {
        o.ta_format = field(lc9, 16, 2);
        o.ta_length = field(lc9, 18, 5);
        o.ta_nbits = 49;
        for (i, b) in o.ta_bits[..49].iter_mut().enumerate() {
            *b = get_field(lc9, 23 + i, 1) as u8;
        }
    } else if o.fid == 0 && (RDOF_FLCO_TA_BLOCK1..=RDOF_FLCO_TA_BLOCK3).contains(&o.flco) {
        o.ta_nbits = 56;
        for (i, b) in o.ta_bits.iter_mut().enumerate() {
            *b = get_field(lc9, 16 + i, 1) as u8;
        }
    }
}

#[no_mangle]
pub extern "C" fn rdof_decode_voice_lc(burst: &Burst, data_type: c_int, out: &mut RdofFullLc) -> bool {
    let (lc12, _) = bptc(burst);
    let mask =
        if data_type == RDOF_DT_VOICE_LC_HEADER { ffi::RDOF_MASK_VOICE_LC_HEADER } else { ffi::RDOF_MASK_TERMINATOR_LC };
    if !unsafe { ffi::rdof_check_rs129(lc12.as_ptr(), mask) } {
        return false;
    }
    rdof_parse_full_lc(lc12[..9].try_into().unwrap(), out);
    true
}

/// Opcodes with target in octets 4-6 and source in 7-9 (ETSI -2 7.1.2 / -4).
fn addressed_csbk(op: c_int) -> bool {
    matches!(op, 0x04 | 0x05 | 0x1F | 0x20 | 0x21 | 0x24 | 0x38 | RDOF_CSBK_PREAMBLE)
}

/// 24 bits big endian from p[0..3].
fn be24(p: &[u8]) -> i32 {
    i32::from_be_bytes([0, p[0], p[1], p[2]])
}

#[no_mangle]
pub extern "C" fn rdof_decode_csbk(burst: &Burst, c: &mut RdofCsbk) {
    let (d, _) = bptc(burst);
    zero(c);
    c.raw = d;
    c.crc_ok = unsafe { ffi::rdof_check_crc16(d.as_ptr(), ffi::RDOF_MASK_CSBK) };
    c.opcode = c_int::from(d[0] & 0x3F);
    c.fid = d[1].into();
    c.last_block = d[0] & 0x80 != 0;
    c.pf = d[0] & 0x40 != 0;
    c.src_id = RDOF_NONE;
    c.dst_id = RDOF_NONE;
    if c.fid == 0 && addressed_csbk(c.opcode) {
        c.dst_id = be24(&d[4..]);
        c.src_id = be24(&d[7..]);
    }
    if c.fid == 0 && c.opcode == RDOF_CSBK_PREAMBLE {
        c.data_follows = d[2] & 0x80 != 0;
        c.group = d[2] & 0x40 != 0;
        c.blocks_to_follow = d[3].into();
    }
}

#[no_mangle]
pub extern "C" fn rdof_parse_data_header(d: &[u8; 12], h: &mut RdofDataHeader) {
    zero(h);
    h.raw = *d;
    h.dpf = field(d, 4, 4);
    h.sap = field(d, 8, 4);
    h.group = get_field(d, 0, 1) != 0;
    h.response_requested = get_field(d, 1, 1) != 0;
    h.dst_id = get_field(d, 16, 24) as u32;
    h.src_id = get_field(d, 40, 24) as u32;
    h.crc_ok = unsafe { ffi::rdof_check_crc16(d.as_ptr(), ffi::RDOF_MASK_DATA_HEADER) };
    h.full_message = true;
    for f in [&mut h.fsn, &mut h.ns, &mut h.udt_format, &mut h.udt_opcode, &mut h.dd_format] {
        *f = RDOF_NONE;
    }
    for f in [&mut h.response_class, &mut h.response_type, &mut h.response_status, &mut h.mfid] {
        *f = RDOF_NONE;
    }
    match h.dpf {
        RDOF_DPF_UNCONFIRMED | RDOF_DPF_CONFIRMED => {
            h.pad_octets = (field(d, 3, 1) << 4) | field(d, 12, 4);
            h.full_message = get_field(d, 64, 1) != 0;
            h.blocks = field(d, 65, 7);
            h.fsn = field(d, 76, 4);
            if h.dpf == RDOF_DPF_CONFIRMED {
                h.ns = field(d, 73, 3);
            }
        }
        RDOF_DPF_RESPONSE => {
            h.blocks = field(d, 65, 7);
            h.response_class = field(d, 72, 2);
            h.response_type = field(d, 74, 3);
            h.response_status = field(d, 77, 3);
        }
        RDOF_DPF_SHORT_DATA_DEFINED | RDOF_DPF_SHORT_DATA_RAW => {
            h.blocks = (field(d, 2, 2) << 4) | field(d, 12, 4);
            if h.dpf == RDOF_DPF_SHORT_DATA_DEFINED {
                h.dd_format = field(d, 64, 6);
                h.full_message = get_field(d, 71, 1) != 0;
                h.bit_padding = field(d, 72, 8);
            }
        }
        RDOF_DPF_UDT => {
            h.udt_format = field(d, 12, 4);
            h.pad_nibbles = field(d, 64, 5);
            h.blocks = field(d, 70, 2) + 1; // "appended blocks" is stored minus one
            h.udt_opcode = field(d, 74, 6);
        }
        RDOF_DPF_PROPRIETARY => {
            h.mfid = d[1].into();
            h.src_id = 0;
            h.dst_id = 0;
        }
        _ => {}
    }
}

#[no_mangle]
pub extern "C" fn rdof_decode_data_header(burst: &Burst, out: &mut RdofDataHeader) {
    rdof_parse_data_header(&bptc(burst).0, out);
}

#[no_mangle]
pub extern "C" fn rdof_decode_data_block(burst: &Burst, data_type: c_int, b: &mut RdofDataBlock) -> bool {
    zero(b);
    b.data_type = data_type;
    match data_type {
        RDOF_DT_RATE_12 => {
            let (d, ok) = bptc(burst);
            b.payload[..12].copy_from_slice(&d);
            b.fec_ok = ok;
            b.len = 12;
        }
        RDOF_DT_RATE_34 => {
            b.errors = unsafe { ffi::rdof_trellis34_decode(burst.as_ptr(), b.payload.as_mut_ptr()) };
            b.fec_ok = b.errors == 0;
            b.len = 18;
        }
        RDOF_DT_RATE_1 => {
            unsafe { ffi::rdof_rate1_payload(burst.as_ptr(), b.payload.as_mut_ptr()) };
            b.fec_ok = true;
            b.len = 24;
        }
        _ => return false,
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_split_confirmed(
    block: &RdofDataBlock,
    dbsn: &mut c_int,
    body: *mut u8,
    body_len: &mut c_int,
) -> bool {
    let mask = match block.data_type {
        RDOF_DT_RATE_12 => ffi::RDOF_MASK_CRC9_RATE12,
        RDOF_DT_RATE_34 => ffi::RDOF_MASK_CRC9_RATE34,
        _ => ffi::RDOF_MASK_CRC9_RATE1,
    };
    let n = field(&block.payload, 0, 7);
    let crc9 = field(&block.payload, 7, 9);
    let Some(src) = usize::try_from(block.len - 2).ok().and_then(|len| block.payload[2..].get(..len)) else {
        // not a data block (rdof_decode_data_block's false leaves len 0)
        *dbsn = 0;
        *body_len = 0;
        return false;
    };
    slice_mut(body, src.len()).copy_from_slice(src);
    *dbsn = n;
    *body_len = src.len() as c_int;
    ffi::rdof_check_crc9(body, src.len(), n, crc9, mask)
}
