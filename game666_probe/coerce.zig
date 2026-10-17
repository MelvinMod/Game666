const std = @import("std");

pub fn main() void {
    const a: i32 = 5;
    const b: u32 = a;
    _ = b;
    var x: u32 = 7;
    if (x < a) {}
    const arr = [_]u8{0} ** 4;
    if (x < arr.len) {}
    const c: c_int = 3;
    var y: c_uint = 9;
    if (y < c) {}
    switch (y) {
        c => {},
        else => {},
    }
    const d: u32 = c;
    _ = d;
    x = a;
    _ = x;
}
