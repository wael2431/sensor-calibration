//! Polynomial calibration validator: decoding, validation, CRC.
//! The polynomial is evaluated in C (calib_eval.c); Rust only guarantees that a received
//! blob is safe before it replaces the active calibration.
//! `no_std` for the ESP32-C6 target, testable on a PC with `cargo test`.
#![cfg_attr(not(test), no_std)]

use core::slice;

// ---------------------------------------------------------------- constantes

pub const CAL_OK: i32 = 0;
pub const CAL_ERR_NULL: i32 = -1;
pub const CAL_ERR_LEN: i32 = -2;
pub const CAL_ERR_MAGIC: i32 = -3;
pub const CAL_ERR_VERSION: i32 = -4;
pub const CAL_ERR_CRC: i32 = -5;
pub const CAL_ERR_CHANNELS: i32 = -6;
pub const CAL_ERR_RANGE: i32 = -7;
pub const CAL_ERR_DIVERGES: i32 = -8;

pub const CAL_MAGIC: u32 = 0x324C_4143; // "CAL2" in little-endian
pub const CAL_VERSION: u8 = 2;
pub const CAL_MAX_CHANNELS: usize = 2;
pub const CAL_POLY_COEFFS: usize = 4;
pub const CAL_HEADER_LEN: usize = 12;
pub const CAL_CH_LEN: usize = 4 * 4 + CAL_POLY_COEFFS * 4; // 32
pub const CAL_BLOB_LEN: usize = CAL_HEADER_LEN + CAL_MAX_CHANNELS * CAL_CH_LEN + 4; // 80
const Q: u32 = 16;

// -------------------------------------------------------------------- types

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CalChannel {
    pub a_min: i32,
    pub a_max: i32,
    pub y_min: i32,
    pub y_max: i32,
    pub c: [i32; CAL_POLY_COEFFS],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Cal {
    pub revision: u32,
    pub n_channels: u8,
    pub _pad: [u8; 3],
    pub ch: [CalChannel; CAL_MAX_CHANNELS],
}

impl Cal {
    /// Identity polynomial: y = a_min + (a_max - a_min) * x  (= a).
    /// Channel 0: temperature -40..85 °C; channel 1: humidity 0..100 %RH (units of 0.01).
    pub const fn neutral() -> Cal {
        Cal {
            revision: 0,
            n_channels: CAL_MAX_CHANNELS as u8,
            _pad: [0; 3],
            ch: [
                CalChannel {
                    a_min: -4000,
                    a_max: 8500,
                    y_min: -4000,
                    y_max: 8500,
                    c: [-4000 * 65536, 12500 * 65536, 0, 0],
                },
                CalChannel {
                    a_min: 0,
                    a_max: 10000,
                    y_min: 0,
                    y_max: 10000,
                    c: [0, 10000 * 65536, 0, 0],
                },
            ],
        }
    }
}

// ------------------------------------------------------------------ logique

pub fn crc32(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            let mask = (crc & 1).wrapping_neg();
            crc = (crc >> 1) ^ (0xEDB8_8320 & mask);
        }
    }
    !crc
}

fn rd_u32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

fn rd_i32(b: &[u8], o: usize) -> i32 {
    rd_u32(b, o) as i32
}

/// Horner in Q16.16 with 64-bit intermediates (same arithmetic as calib_eval.c).
/// `x`: normalized input, Q16.16 in [0, 65536]. Returns y in Q16.16.
fn horner_q16(c: &[i32; CAL_POLY_COEFFS], x: i64) -> i64 {
    let mut r = c[CAL_POLY_COEFFS - 1] as i64;
    for k in (0..CAL_POLY_COEFFS - 1).rev() {
        r = ((r * x) >> Q) + c[k] as i64;
    }
    r
}

fn validate_channel(c: &CalChannel) -> Result<(), i32> {
    let span_a = c.a_max as i64 - c.a_min as i64;
    if span_a <= 0 || span_a > i32::MAX as i64 {
        return Err(CAL_ERR_RANGE);
    }
    if c.y_min >= c.y_max {
        return Err(CAL_ERR_RANGE);
    }
    // The polynomial must stay within a reasonable range over the whole input [0,1]:
    // at most one output span beyond the bounds [y_min, y_max].
    let span_y = c.y_max as i64 - c.y_min as i64;
    let lo = c.y_min as i64 - span_y;
    let hi = c.y_max as i64 + span_y;
    for k in 0..=32i64 {
        let x = k * (1i64 << Q) / 32;
        let y = (horner_q16(&c.c, x) + (1i64 << (Q - 1))) >> Q;
        if y < lo || y > hi {
            return Err(CAL_ERR_DIVERGES);
        }
    }
    Ok(())
}

pub fn validate(cal: &Cal) -> Result<(), i32> {
    if cal.n_channels as usize != CAL_MAX_CHANNELS {
        return Err(CAL_ERR_CHANNELS);
    }
    for c in cal.ch.iter() {
        validate_channel(c)?;
    }
    Ok(())
}

pub fn parse(b: &[u8]) -> Result<Cal, i32> {
    if b.len() != CAL_BLOB_LEN {
        return Err(CAL_ERR_LEN);
    }
    if rd_u32(b, 0) != CAL_MAGIC {
        return Err(CAL_ERR_MAGIC);
    }
    if b[4] != CAL_VERSION {
        return Err(CAL_ERR_VERSION);
    }
    let body = CAL_BLOB_LEN - 4;
    if crc32(&b[..body]) != rd_u32(b, body) {
        return Err(CAL_ERR_CRC);
    }
    let mut cal = Cal::neutral();
    cal.n_channels = b[5];
    cal.revision = rd_u32(b, 8);
    for i in 0..CAL_MAX_CHANNELS {
        let o = CAL_HEADER_LEN + i * CAL_CH_LEN;
        let ch = &mut cal.ch[i];
        ch.a_min = rd_i32(b, o);
        ch.a_max = rd_i32(b, o + 4);
        ch.y_min = rd_i32(b, o + 8);
        ch.y_max = rd_i32(b, o + 12);
        for k in 0..CAL_POLY_COEFFS {
            ch.c[k] = rd_i32(b, o + 16 + 4 * k);
        }
    }
    validate(&cal)?;
    Ok(cal)
}

pub fn serialize(cal: &Cal, out: &mut [u8]) -> Result<usize, i32> {
    validate(cal)?;
    if out.len() < CAL_BLOB_LEN {
        return Err(CAL_ERR_LEN);
    }
    out[0..4].copy_from_slice(&CAL_MAGIC.to_le_bytes());
    out[4] = CAL_VERSION;
    out[5] = cal.n_channels;
    out[6] = 0;
    out[7] = 0;
    out[8..12].copy_from_slice(&cal.revision.to_le_bytes());
    for i in 0..CAL_MAX_CHANNELS {
        let o = CAL_HEADER_LEN + i * CAL_CH_LEN;
        let ch = &cal.ch[i];
        out[o..o + 4].copy_from_slice(&ch.a_min.to_le_bytes());
        out[o + 4..o + 8].copy_from_slice(&ch.a_max.to_le_bytes());
        out[o + 8..o + 12].copy_from_slice(&ch.y_min.to_le_bytes());
        out[o + 12..o + 16].copy_from_slice(&ch.y_max.to_le_bytes());
        for k in 0..CAL_POLY_COEFFS {
            out[o + 16 + 4 * k..o + 20 + 4 * k].copy_from_slice(&ch.c[k].to_le_bytes());
        }
    }
    let body = CAL_BLOB_LEN - 4;
    let crc = crc32(&out[..body]);
    out[body..CAL_BLOB_LEN].copy_from_slice(&crc.to_le_bytes());
    Ok(CAL_BLOB_LEN)
}

// ------------------------------------------------------------------ API C

#[no_mangle]
pub unsafe extern "C" fn cal_default(out: *mut Cal) {
    if !out.is_null() {
        *out = Cal::neutral();
    }
}

#[no_mangle]
pub unsafe extern "C" fn cal_parse(buf: *const u8, len: usize, out: *mut Cal) -> i32 {
    if buf.is_null() || out.is_null() {
        return CAL_ERR_NULL;
    }
    match parse(slice::from_raw_parts(buf, len)) {
        Ok(c) => {
            *out = c;
            CAL_OK
        }
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn cal_validate(cal: *const Cal) -> i32 {
    if cal.is_null() {
        return CAL_ERR_NULL;
    }
    match validate(&*cal) {
        Ok(()) => CAL_OK,
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn cal_serialize(cal: *const Cal, buf: *mut u8, cap: usize) -> usize {
    if cal.is_null() || buf.is_null() {
        return 0;
    }
    serialize(&*cal, slice::from_raw_parts_mut(buf, cap)).unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn cal_crc32(data: *const u8, len: usize) -> u32 {
    if data.is_null() {
        return crc32(&[]);
    }
    crc32(slice::from_raw_parts(data, len))
}

// ------------------------------------------------------------ panic (cible)

#[cfg(not(test))]
extern "C" {
    fn abort() -> !;
}

#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { abort() }
}

// -------------------------------------------------------------------- tests

#[cfg(test)]
mod tests {
    use super::*;

    /// Realistic polynomial: BME280 ≈ 1.02 * DHT11 − 0.3 °C with a slight curvature.
    fn sample() -> Cal {
        let mut c = Cal::neutral();
        c.revision = 7;
        c.ch[0] = CalChannel {
            a_min: 1000,
            a_max: 3500,
            y_min: 900,
            y_max: 3700,
            c: [1030 * 65536, 2450 * 65536, 30 * 65536, -20 * 65536],
        };
        c
    }

    #[test]
    fn sizes_match_c_layout() {
        assert_eq!(core::mem::size_of::<CalChannel>(), CAL_CH_LEN);
        assert_eq!(core::mem::size_of::<Cal>(), 72);
        assert_eq!(CAL_BLOB_LEN, 80);
    }

    #[test]
    fn crc32_check_value() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(crc32(&[]), 0);
    }

    #[test]
    fn neutral_is_valid_and_identity() {
        let n = Cal::neutral();
        assert_eq!(validate(&n), Ok(()));
        for ch in n.ch.iter() {
            let span = (ch.a_max - ch.a_min) as i64;
            for a in [ch.a_min as i64, 0i64.max(ch.a_min as i64) + span / 3, ch.a_max as i64] {
                let x = ((a - ch.a_min as i64) << Q) / span;
                let y = (horner_q16(&ch.c, x) + (1 << (Q - 1))) >> Q;
                assert!((y - a).abs() <= 1, "identity: a={} y={}", a, y);
            }
        }
    }

    #[test]
    fn horner_known_values() {
        // y = 10 + 20 x  (Q16) at x = 0.5  -> 20
        let c = [10 << 16, 20 << 16, 0, 0];
        assert_eq!((horner_q16(&c, 32768) + 32768) >> 16, 20);
        // y = x^3 at x = 0.5 -> 0.125 (Q16 = 8192)
        let c = [0, 0, 0, 1 << 16];
        assert_eq!(horner_q16(&c, 32768), 8192);
    }

    #[test]
    fn roundtrip() {
        let c = sample();
        let mut buf = [0u8; CAL_BLOB_LEN];
        assert_eq!(serialize(&c, &mut buf), Ok(CAL_BLOB_LEN));
        assert_eq!(parse(&buf), Ok(c));
    }

    #[test]
    fn rejects_bad_length() {
        let buf = [0u8; CAL_BLOB_LEN - 1];
        assert_eq!(parse(&buf), Err(CAL_ERR_LEN));
    }

    #[test]
    fn rejects_bad_magic_version_crc() {
        let mut buf = [0u8; CAL_BLOB_LEN];
        serialize(&sample(), &mut buf).unwrap();

        let mut b = buf;
        b[0] ^= 1;
        assert_eq!(parse(&b), Err(CAL_ERR_MAGIC));

        let mut b = buf;
        b[4] = 9;
        assert_eq!(parse(&b), Err(CAL_ERR_VERSION));

        let mut b = buf;
        b[30] ^= 0x40; // one bit in the body
        assert_eq!(parse(&b), Err(CAL_ERR_CRC));
    }

    #[test]
    fn rejects_wrong_channel_count() {
        let mut c = sample();
        c.n_channels = 3;
        assert_eq!(validate(&c), Err(CAL_ERR_CHANNELS));
    }

    #[test]
    fn rejects_bad_ranges() {
        let mut c = sample();
        c.ch[0].a_max = c.ch[0].a_min;
        assert_eq!(validate(&c), Err(CAL_ERR_RANGE));

        let mut c = sample();
        c.ch[1].y_min = c.ch[1].y_max;
        assert_eq!(validate(&c), Err(CAL_ERR_RANGE));

        let mut c = sample();
        c.ch[0].a_min = i32::MIN;
        c.ch[0].a_max = i32::MAX;
        assert_eq!(validate(&c), Err(CAL_ERR_RANGE));
        let mut buf = [0u8; CAL_BLOB_LEN];
        assert_eq!(serialize(&c, &mut buf), Err(CAL_ERR_RANGE));
    }

    #[test]
    fn rejects_diverging_polynomial() {
        let mut c = sample();
        c.ch[0].c[3] = i32::MAX; // huge x^3 term
        assert_eq!(validate(&c), Err(CAL_ERR_DIVERGES));
    }

    #[test]
    fn c_api_null_checks() {
        unsafe {
            let mut c = Cal::neutral();
            assert_eq!(cal_parse(core::ptr::null(), 0, &mut c), CAL_ERR_NULL);
            assert_eq!(cal_validate(core::ptr::null()), CAL_ERR_NULL);
            assert_eq!(cal_serialize(core::ptr::null(), core::ptr::null_mut(), 0), 0);
        }
    }
}
