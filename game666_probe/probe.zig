const std = @import("std");

const Block = enum(u8) { air = 0, stone = 1 };

pub fn main() !void {
    var gpa_state: std.heap.DebugAllocator(.{}) = .init;
    defer _ = gpa_state.deinit();
    const allocator = gpa_state.allocator();

    var list: std.ArrayList(u32) = .empty;
    defer list.deinit(allocator);
    try list.append(allocator, 7);

    var map = std.AutoHashMap(u64, u32).init(allocator);
    defer map.deinit();
    try map.put(1, 2);
    const p = map.getPtr(1).?;
    p.* = 3;
    const c: ?u32 = map.get(1);

    var ts: std.os.linux.timespec = undefined;
    _ = std.os.linux.clock_gettime(.MONOTONIC, &ts);
    const ms: u64 = @as(u64, @intCast(ts.sec)) * 1000 + @as(u64, @intCast(ts.nsec)) / std.time.ns_per_ms;

    const req = std.os.linux.timespec{ .sec = 0, .nsec = 2 * std.time.ns_per_ms };
    _ = std.os.linux.nanosleep(&req, null);

    const x: f32 = 1.5;
    const b: Block = .stone;
    const from: ?Block = std.enums.fromInt(Block, 1);
    const key: u64 = (@as(u64, @intCast(@as(u32, @bitCast(@as(i32, -1))))) << 32) |
        @as(u64, @intCast(@as(u32, @bitCast(@as(i32, -2)))));
    const fx: i32 = @intFromFloat(@floor(x));
    const clamped: f32 = std.math.clamp(x, 0.0, 1.0);
    const wrapped: f32 = @mod(-0.5, std.math.tau);

    std.debug.print("ms={d} list={d} map={d} get={?d} f={d:.2} bool={} str={s}\n", .{ ms, list.items[0], p.*, c, x, true, "ok" });
    std.debug.print("enum={s} from={?s} key={d} fx={d} clamp={d:.2} wrap={d:.3}\n", .{ @tagName(b), if (from) |f| @tagName(f) else null, key, fx, clamped, wrapped });
}
