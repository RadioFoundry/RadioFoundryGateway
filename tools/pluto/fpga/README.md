# PlutoSDR bitstreams

`rfg-pluto-<board>.bit.bin`: the PL design with rdofradio's FPGA channelizer (`--fpga-chz`) and TX interpolator
(`--fpga-duc`) beside ADI's AD9361 interface, one per board (`sdr/include/rdof/sdr/plutofpga.h` tells them apart).
bootgen's `.bin`, for the Linux FPGA manager. Built in rfg-pluto from its `hdl/`; `MANIFEST` gives the commit each
was built from.

| Board | File |
|---|---|
| Zynq 7010 clone with 1 GB of 32-bit DDR, Ethernet and LVDS (fishball7010) | `rfg-pluto-fishball7010.bit.bin` |

License: MIT for rfg-pluto's own design ([LICENSE](LICENSE)), ADI's BSD for the parts from ADI's HDL
([LICENSE_ADIBSD](LICENSE_ADIBSD)).

`rdofradio` loads the board's at start ([docs/reference.md](../../../docs/reference.md), "The bitstream").
`install.sh` in the bundle copies it to `rdof/` on the SD card's boot partition.
