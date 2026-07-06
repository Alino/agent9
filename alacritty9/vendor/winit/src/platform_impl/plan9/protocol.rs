//! Wire protocol between gl9win2 and the app. Source of truth: alacritty9/PROTOCOL.md.
#![allow(dead_code)]

/// Event records on fd 0 (win -> app) are fixed 16-byte big-endian records.
pub const RECORD_SIZE: usize = 16;

// Record types.
pub const EV_KEY: u8 = 1;
pub const EV_MOUSE_MOVE: u8 = 2;
pub const EV_MOUSE_BUTTON: u8 = 3;
pub const EV_SCROLL: u8 = 4;
pub const EV_RESIZE: u8 = 5;
pub const EV_FOCUS: u8 = 6;
pub const EV_QUIT: u8 = 7;

// Modifier bitmask.
pub const MOD_SHIFT: u16 = 1 << 0;
pub const MOD_CTRL: u16 = 1 << 1;
pub const MOD_ALT: u16 = 1 << 2;
pub const MOD_SUPER: u16 = 1 << 3;

// Mouse buttons (record field `a` of EV_MOUSE_BUTTON).
pub const BTN_LEFT: u32 = 1;
pub const BTN_MIDDLE: u32 = 2;
pub const BTN_RIGHT: u32 = 3;

// Plan 9 key runes (/sys/include/keyboard.h). Printable keys carry their rune.
pub const KBS: u32 = 0x08;
pub const KTAB: u32 = 0x09;
pub const KNL: u32 = 0x0A;
pub const KESC: u32 = 0x1B;
pub const KSPACE: u32 = 0x20;
pub const KDEL: u32 = 0x7F;
/// Yes, Kdown really is 0x80 on Plan 9.
pub const KDOWN: u32 = 0x80;
/// KF|n == KF1 + n - 1 for n in 1..=12.
pub const KF1: u32 = 0xF001;
pub const KF12: u32 = 0xF00C;
pub const KHOME: u32 = 0xF00D;
pub const KUP: u32 = 0xF00E;
pub const KPGUP: u32 = 0xF00F;
pub const KLEFT: u32 = 0xF011;
pub const KRIGHT: u32 = 0xF012;
pub const KPGDOWN: u32 = 0xF013;
pub const KINS: u32 = 0xF014;
pub const KALT: u32 = 0xF015;
pub const KSHIFT: u32 = 0xF016;
pub const KCTL: u32 = 0xF017;
pub const KEND: u32 = 0xF018;

/// One decoded event record.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Record {
    pub typ: u8,
    pub state: u8,
    pub modifiers: u16,
    pub a: u32,
    pub b: u32,
}

impl Record {
    pub fn parse(buf: &[u8; RECORD_SIZE]) -> Self {
        Self {
            typ: buf[0],
            state: buf[1],
            modifiers: u16::from_be_bytes([buf[2], buf[3]]),
            a: u32::from_be_bytes([buf[4], buf[5], buf[6], buf[7]]),
            b: u32::from_be_bytes([buf[8], buf[9], buf[10], buf[11]]),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_resize_record() {
        let mut buf = [0u8; RECORD_SIZE];
        buf[0] = EV_RESIZE;
        buf[2..4].copy_from_slice(&(MOD_SHIFT | MOD_CTRL).to_be_bytes());
        buf[4..8].copy_from_slice(&800u32.to_be_bytes());
        buf[8..12].copy_from_slice(&600u32.to_be_bytes());
        let r = Record::parse(&buf);
        assert_eq!(r, Record { typ: EV_RESIZE, state: 0, modifiers: 3, a: 800, b: 600 });
    }
}
