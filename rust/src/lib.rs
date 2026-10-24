// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Firmware modules in Rust, linked into the C build in place of their C files (rust/README.md).
//! no_std: no allocator, no threads; each module exports the C header's functions under the same names.
#![no_std]
// The exported functions' safety contract is their C header's (valid pointers, the lengths it gives).
#![allow(clippy::missing_safety_doc)]

pub mod ambe;
pub mod benchcfg;
pub mod benchtx;
pub mod bits;
pub mod config;
mod ffi;
pub mod hblink;
pub mod jitter;
pub mod json;
pub mod jsonp;
pub mod layer2;
mod layout;
pub mod mmdvm;
pub mod modem;
pub mod netcalls;
pub mod oled;
pub mod pdu;
pub mod rawburst;
pub mod repeater;
pub mod slotgrid;
pub mod status;
mod text;
pub mod tracker;
pub mod txvr;
pub mod usbmatch;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    // A panic is a bug (an index out of range); stop as a C assert would.
    unsafe { ffi::abort() }
}
