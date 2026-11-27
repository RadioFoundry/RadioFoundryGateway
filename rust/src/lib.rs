// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Firmware modules in Rust, linked into the C build in place of their C files (rust/README.md).
//! no_std: no allocator, no threads; each module exports the C header's functions under the same names.
#![cfg_attr(not(test), no_std)]
// The exported functions' safety contract is their C header's (valid pointers, the lengths it gives).
#![allow(clippy::missing_safety_doc)]

#[cfg(feature = "legacy")]
pub mod ambe;
#[cfg(feature = "legacy")]
pub mod benchcfg;
#[cfg(feature = "legacy")]
pub mod benchtx;
#[cfg(feature = "legacy")]
pub mod bits;
#[cfg(feature = "legacy")]
pub mod config;
#[cfg(feature = "legacy")]
mod ffi;
#[cfg(feature = "legacy")]
pub mod hblink;
#[cfg(feature = "legacy")]
pub mod jitter;
#[cfg(feature = "legacy")]
pub mod json;
#[cfg(feature = "legacy")]
pub mod jsonp;
#[cfg(feature = "legacy")]
pub mod lastheard;
#[cfg(feature = "legacy")]
pub mod layer2;
#[cfg(feature = "legacy")]
mod layout;
#[cfg(feature = "legacy")]
pub mod mmdvm;
#[cfg(feature = "legacy")]
pub mod modem;
#[cfg(feature = "legacy")]
pub mod mrelay;
#[cfg(feature = "legacy")]
pub mod netcalls;
#[cfg(feature = "legacy")]
pub mod oled;
#[cfg(feature = "legacy")]
pub mod parrot;
#[cfg(feature = "legacy")]
pub mod pdu;
#[cfg(feature = "legacy")]
mod plan;
#[cfg(feature = "legacy")]
pub mod rawburst;
#[cfg(feature = "legacy")]
pub mod repeater;
#[cfg(feature = "legacy")]
pub mod sigiq;
#[cfg(feature = "legacy")]
pub mod sigmulti;
#[cfg(feature = "legacy")]
pub mod signoise;
#[cfg(feature = "legacy")]
pub mod slotgrid;
#[cfg(feature = "legacy")]
pub mod status;
#[cfg(feature = "legacy")]
mod text;
#[cfg(feature = "legacy")]
pub mod tracker;
#[cfg(feature = "legacy")]
pub mod txvr;
#[cfg(feature = "legacy")]
pub mod usbmatch;

#[cfg(feature = "t3")]
pub mod t3air;
#[cfg(feature = "t3")]
pub mod t3hat;
#[cfg(feature = "t3")]
pub mod t3auth;
#[cfg(feature = "t3")]
pub mod t3ms;
#[cfg(feature = "t3")]
pub mod t3pdnet;
#[cfg(feature = "t3")]
pub mod t3pdrate;
#[cfg(feature = "t3")]
pub mod t3pdu;
#[cfg(feature = "t3")]
pub mod t3rc;
#[cfg(feature = "t3")]
pub mod t3site;
#[cfg(feature = "t3")]
pub mod t3udt;
#[cfg(all(test, feature = "t3"))]
mod t3authtests;
#[cfg(all(test, feature = "t3"))]
mod t3ctltests;
#[cfg(all(test, feature = "t3"))]
mod t3tests;
#[cfg(all(test, feature = "t3"))]
mod t3spec_bits;
#[cfg(all(test, feature = "t3"))]
mod t3spec_ms;
#[cfg(all(test, feature = "t3"))]
mod t3spec_pdu;
#[cfg(all(test, feature = "t3"))]
mod t3spec_site;
#[cfg(all(test, feature = "t3"))]
mod t3sdtests;
#[cfg(all(test, feature = "t3"))]
mod t3pdtests;
#[cfg(all(test, feature = "t3"))]
mod t3pdnettests;
#[cfg(all(test, feature = "t3"))]
mod t3tgtests;
#[cfg(all(test, feature = "t3"))]
mod t3sutests;
#[cfg(all(test, feature = "t3"))]
mod t3intests;

// The Tier III site around the controller (the tier3 feature; docs/spec/fw/tier3.md 9). Their C headers are the
// contract: rdof/t3rf.h, rdof/t3cfg.h, rdof/t3unit.h, rdof/t3sim.h, rdof/sdr/t3split.h.
#[cfg(feature = "tier3")]
pub mod t3cfg;
#[cfg(feature = "tier3")]
pub mod t3rf;
#[cfg(feature = "tier3")]
pub mod t3sim;
#[cfg(feature = "tier3")]
pub mod t3split;
#[cfg(feature = "tier3")]
pub mod t3unit;

#[cfg(not(any(feature = "legacy", test)))]
mod ffi {
    extern "C" {
        pub fn abort() -> !;
    }
}

#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    // A panic is a bug (an index out of range); stop as a C assert would.
    unsafe { ffi::abort() }
}
