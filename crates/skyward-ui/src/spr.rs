//! Qualcomm SPR v2 payload for Xiaomi 15's O3 panel.
//!
//! Matches drm_msm_spr_init_cfg (304 bytes) and the installed
//! spr_cfg_xiaomi_o3_42_0d_0a_cmd_mode_dsc_dsi_panel.xml, mode 0.
//! Verified against the vendor's generated payload and on the device.
//! Explicit little-endian offsets avoid Rust layout and padding changes.

pub fn dada_o3() -> [u8; 304] {
    let mut blob = [0; 304];
    fn u16s(blob: &mut [u8], offset: usize, values: &[u16]) {
        for (i, value) in values.iter().enumerate() {
            blob[offset + i * 2..offset + i * 2 + 2].copy_from_slice(&value.to_le_bytes());
        }
    }
    // Enable SPR, 2DAvg filter, phase increment 8 and repeat 2.
    u16s(&mut blob, 8, &[1, 0, 0, 1, 0, 8, 2, 4, 0, 0]);
    // Pentile packing, RGBW coefficients, and adaptive filter coefficients.
    u16s(&mut blob, 32, &[1, 0, 1, 0]);
    u16s(&mut blob, 48, &[341, 341, 341, 0]);
    u16s(&mut blob, 56, &[16; 5]);
    // Vendor's 12-bit linear lookup table, including its rounding.
    const LUT: [u16; 32] = [
        0, 132, 264, 396, 529, 661, 793, 925, 1057, 1189, 1321, 1453,
        1586, 1718, 1850, 1982, 2114, 2246, 2378, 2510, 2643, 2775,
        2907, 3039, 3171, 3303, 3435, 3567, 3700, 3832, 3964, 4095,
    ];
    u16s(&mut blob, 66, &LUT);
    for (i, value) in [0i32, 256, 256, 0].into_iter().cycle().take(16).enumerate() {
        blob[132 + i * 4..136 + i * 4].copy_from_slice(&value.to_le_bytes());
    }
    // Six phase offsets each for red, green, blue, and white.
    for (i, value) in [(0, -2i32), (1, 2), (12, 2), (13, -2)] {
        blob[196 + i * 4..200 + i * 4].copy_from_slice(&value.to_le_bytes());
    }
    u16s(&mut blob, 292, &[1]);
    // PBA XML 7,7,7,2,1,2,1, encoded in kernel channel/order layout.
    blob[294..301].copy_from_slice(&[7, 7, 7, 4, 2, 4, 2]);
    blob
}

