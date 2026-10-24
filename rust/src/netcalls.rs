// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! What a roamer or repeater transmits, reported as the network calls they are (rdof/netcalls.h). A port of
//! common/src/netcalls.c.

use core::ffi::{c_char, c_int};

use crate::ffi::RDOF_BURST_BYTES;
use crate::json::RdofJson;
use crate::mmdvm::{rdof_control_to_dt, RDOF_SYNC_DATA};
use crate::pdu::{rdof_decode_csbk, RdofCsbk};
use crate::rawburst::{rdof_rawburst_json, RdofRawburst};
use crate::text::{c_bytes, c_out, mhz, Out};

type Burst = [u8; RDOF_BURST_BYTES];

/// rdof_tx_call_info_t
#[repr(C)]
pub struct RdofTxCallInfo {
    pub stream_id: [u8; 4],
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub freq: u32,
    pub cc: c_int,
    pub slot: c_int,
    pub has_csbk: bool,
    pub csbk: Burst,
    pub frames: c_int,
}

/// rdof_netcalls_t
#[repr(C)]
pub struct RdofNetcalls {
    pub has_last: [bool; 3],
    pub last_control: [u8; 3],
    pub last_burst: [Burst; 3],
}

impl RdofTxCallInfo {
    /// The slot it's reported on: the roamer's is 2.
    fn slot(&self) -> usize {
        if self.slot == 1 {
            1
        } else {
            2
        }
    }

    /// The call's addressing and channel, as a call start and a lone CSBK both report them.
    fn who(&self, j: &mut RdofJson) {
        let mut f = [0u8; 64];
        j.int(c"src_id", self.src.into());
        j.int(c"dst_id", self.dst.into());
        j.str(c"group", if self.group { b"yes" } else { b"no" });
        let mut info = [0u8; 16];
        let n = {
            let mut o = Out::new(&mut info);
            o.int(self.src.into(), 0);
            o.n
        };
        j.str(c"src_info", &info[..n]);
        if self.has_csbk {
            let mut desc = [0u8; 48];
            let n = csbk_desc(&self.csbk, &mut Out::new(&mut desc));
            j.str(c"csbk_desc", &desc[..n]);
        }
        j.str(c"frequency", mhz(self.freq, &mut f));
        j.int(c"colorCode", self.cc.into());
    }
}

#[no_mangle]
pub extern "C" fn rdof_netcalls_init(n: &mut RdofNetcalls) {
    *n = RdofNetcalls { has_last: [false; 3], last_control: [0; 3], last_burst: [[0; RDOF_BURST_BYTES]; 3] };
}

/// What a CSBK is, in the words receivers use for it; the length written.
fn csbk_desc(burst: &Burst, o: &mut Out) -> usize {
    let mut c = RdofCsbk::default();
    rdof_decode_csbk(burst, &mut c);
    let d: &[u8] = match c.opcode {
        0x04 => b"Unit to Unit Voice Service Request",
        0x05 => b"Unit to Unit Voice Answer Response",
        0x26 => b"Negative Acknowledgment Response",
        0x3D => b"Preamble",
        0x1F => b"Call Alert",
        0x20 => b"Call Alert Ack",
        0x24 if c.raw[3] == 0x80 => b"Radio Check",
        0x24 => b"Radio Check Ack",
        0x27 => b"Call Emergency",
        op => {
            // "CSBK 0x%02X" (the opcode is 6 bits)
            const HEX: &[u8; 16] = b"0123456789ABCDEF";
            o.bytes(b"CSBK 0x");
            o.byte(HEX[(op >> 4 & 15) as usize]);
            o.byte(HEX[(op & 15) as usize]);
            return o.n;
        }
    };
    o.bytes(d);
    o.n
}

#[no_mangle]
pub unsafe extern "C" fn rdof_csbk_desc(burst: &Burst, out: *mut c_char, cap: usize) {
    c_out(out, cap, |o| {
        csbk_desc(burst, o);
    });
}

fn iso(t: f64) -> [c_char; 32] {
    let mut ts = [0; 32];
    unsafe { crate::tracker::rdof_iso_time(t, ts.as_mut_ptr()) };
    ts
}

#[no_mangle]
pub unsafe extern "C" fn rdof_netcalls_event(
    n: &mut RdofNetcalls,
    action: *const c_char,
    call: &RdofTxCallInfo,
    duration: f64,
    wall_t: f64,
    out: *mut c_char,
    cap: usize,
) -> bool {
    let start = c_bytes(action) == b"start";
    if call.has_csbk && !start {
        return false; // a lone CSBK is reported once, whole
    }
    let slot = call.slot();
    let ts = iso(wall_t);
    let j = &mut RdofJson::new(out, cap);
    j.open(None);
    j.open(Some(c"DMR"));
    j.str(c"timestamp", c_bytes(ts.as_ptr()));
    j.str(c"source", b"network");
    j.str(c"action", if call.has_csbk { b"csbk" } else { c_bytes(action) });
    j.int(c"slot", slot as i64);
    if start {
        if !call.has_csbk {
            n.has_last[slot] = false;
        }
        call.who(j);
        if !call.has_csbk && call.frames >= 0 {
            j.int(c"frames", call.frames.into());
        }
    } else {
        j.float3(c"duration", duration);
    }
    j.close();
    j.close();
    j.done()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_netcalls_burst_raw(
    n: &mut RdofNetcalls,
    call: &RdofTxCallInfo,
    control: u8,
    burst: &Burst,
    wall_t: f64,
    out: &mut RdofRawburst,
) -> bool {
    if call.has_csbk {
        return false; // reported whole by the event
    }
    let slot = call.slot();
    let (mut dt, mut nn) = (0, 0);
    rdof_control_to_dt(control, &mut dt, &mut nn);
    if control & RDOF_SYNC_DATA != 0
        && n.has_last[slot]
        && n.last_control[slot] == control
        && n.last_burst[slot] == *burst
    {
        return false; // a repeated header or terminator
    }
    n.has_last[slot] = true;
    n.last_control[slot] = control;
    n.last_burst[slot] = *burst;
    *out = RdofRawburst {
        ms: crate::tracker::rdof_time_ms(wall_t),
        slot: slot as c_int,
        network: true,
        dt,
        n: nn,
        addressed: true,
        src: call.src,
        dst: call.dst,
        group: call.group,
        burst: *burst,
        utc: false,
        utc_us: 0,
        utc_slot: 0,
    };
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_netcalls_burst(
    n: &mut RdofNetcalls,
    call: &RdofTxCallInfo,
    control: u8,
    burst: &Burst,
    wall_t: f64,
    out: *mut c_char,
    cap: usize,
) -> bool {
    let mut r = core::mem::zeroed::<RdofRawburst>();
    rdof_netcalls_burst_raw(n, call, control, burst, wall_t, &mut r) && rdof_rawburst_json(&r, out, cap)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_tx_json(
    action: *const c_char,
    call: &RdofTxCallInfo,
    duration: f64,
    wall_t: f64,
    out: *mut c_char,
    cap: usize,
) {
    let (ts, mut f) = (iso(wall_t), [0u8; 64]);
    let action = c_bytes(action);
    let j = &mut RdofJson::new(out, cap);
    j.open(None);
    j.open(Some(c"TX"));
    j.str(c"timestamp", c_bytes(ts.as_ptr()));
    j.str(c"action", action);
    j.int(c"slot", call.slot.into());
    j.int(c"src_id", call.src.into());
    j.int(c"dst_id", call.dst.into());
    j.str(c"group", if call.group { b"yes" } else { b"no" });
    j.str(c"frequency", mhz(call.freq, &mut f));
    j.int(c"colorCode", call.cc.into());
    if action == b"end" {
        j.float3(c"duration", duration);
    }
    j.close();
    j.close();
}
