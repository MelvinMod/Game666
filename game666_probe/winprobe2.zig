const std = @import("std");

const win = @cImport({
    @cDefine("WIN32_LEAN_AND_MEAN", "1");
    @cDefine("_WIN32_WINNT", "0x0601");
    @cInclude("windows.h");
});

comptime {
    var s: []const u8 = "";
    const wc = @typeInfo(win.WNDCLASSA).@"struct";
    for (wc.fields) |f| s = s ++ "WNDCLASS." ++ f.name ++ ": " ++ @typeName(f.type) ++ "\n";
    const msg = @typeInfo(win.MSG).@"struct";
    for (msg.fields) |f| s = s ++ "MSG." ++ f.name ++ ": " ++ @typeName(f.type) ++ "\n";
    s = s ++ "PeekMessageA: " ++ @typeName(@TypeOf(win.PeekMessageA)) ++ "\n";
    s = s ++ "DefWindowProcA: " ++ @typeName(@TypeOf(win.DefWindowProcA)) ++ "\n";
    s = s ++ "RegisterClassA: " ++ @typeName(@TypeOf(win.RegisterClassA)) ++ "\n";
    s = s ++ "GetDC: " ++ @typeName(@TypeOf(win.GetDC)) ++ "\n";
    s = s ++ "ReleaseDC: " ++ @typeName(@TypeOf(win.ReleaseDC)) ++ "\n";
    s = s ++ "DestroyWindow: " ++ @typeName(@TypeOf(win.DestroyWindow)) ++ "\n";
    s = s ++ "PostQuitMessage: " ++ @typeName(@TypeOf(win.PostQuitMessage)) ++ "\n";
    s = s ++ "TranslateMessage: " ++ @typeName(@TypeOf(win.TranslateMessage)) ++ "\n";
    s = s ++ "DispatchMessageA: " ++ @typeName(@TypeOf(win.DispatchMessageA)) ++ "\n";
    s = s ++ "WNDPROC: " ++ @typeName(win.WNDPROC) ++ "\n";
    s = s ++ "HDC: " ++ @typeName(win.HDC) ++ "\n";
    s = s ++ "HWND: " ++ @typeName(win.HWND) ++ "\n";
    @compileError(s);
}

pub fn main() void {}
