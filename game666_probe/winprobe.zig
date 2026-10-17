const std = @import("std");

const win = @cImport({
    @cDefine("WIN32_LEAN_AND_MEAN", "1");
    @cDefine("_WIN32_WINNT", "0x0601");
    @cInclude("windows.h");
});

comptime {
    @compileError("types: " ++
        @typeName(@TypeOf(win.WS_OVERLAPPEDWINDOW)) ++ " | " ++
        @typeName(@TypeOf(win.WS_VISIBLE)) ++ " | " ++
        @typeName(@TypeOf(win.WM_CLOSE)) ++ " | " ++
        @typeName(@TypeOf(win.VK_ESCAPE)) ++ " | " ++
        @typeName(@TypeOf(win.HWND)) ++ " | " ++
        @typeName(@TypeOf(win.WPARAM)) ++ " | " ++
        @typeName(@TypeOf(win.LRESULT)) ++ " | " ++
        @typeName(@TypeOf(win.CW_USEDEFAULT)) ++ " | " ++
        @typeName(@TypeOf(win.PM_REMOVE)) ++ " | " ++
        @typeName(@TypeOf(win.MSG)) ++ " | " ++
        @typeName(@TypeOf(win.WNDCLASSA)) ++ " | " ++
        @typeName(@TypeOf(win.GetTickCount64)) ++ " | " ++
        @typeName(@TypeOf(win.SwapBuffers)) ++ " | " ++
        @typeName(@TypeOf(win.Sleep)) ++ " | " ++
        @typeName(@TypeOf(win.GetModuleHandleA)) ++ " | " ++
        @typeName(@TypeOf(win.CreateWindowExA)) ++ " | " ++
        @typeName(@TypeOf(win.WNDPROC)));
}

pub fn main() void {}
