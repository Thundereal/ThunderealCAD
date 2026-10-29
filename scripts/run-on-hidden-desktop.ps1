# run-on-hidden-desktop.ps1 <command line>
#
# Runs `cmd /c <command line>` on a desktop of its own (WinSta0\fcad-gui-test),
# waits for it and exits with its exit code. A GUI test run this way has real
# windows -- OpenGL and Direct3D work, unlike QT_QPA_PLATFORM=offscreen, where
# on Windows a QOpenGLWidget is "not supported on this platform", the 3D view
# never draws and every saveRenderDump times out -- and none of it shows on
# the user's screen. Nothing there takes input: QTest events still reach the
# widgets, the system pointer and SetCursorPos do not. A box left open is
# unseen as well: the test's own sweeper and the guard's timeout end it
# (scripts/gui-test-guard.py).
#
# Used by scripts/gui-test.cmd, the Windows GUI test driver, by default.
#
# The child inherits this process's environment and current directory; the
# command line does its own redirection (cmd /c "... > log 2>&1").
param([Parameter(Mandatory = $true)][string]$CommandLine)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class HiddenDesktop
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO
    {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute;
        public int dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION
    {
        public IntPtr hProcess, hThread;
        public int dwProcessId, dwThreadId;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateDesktop(string name, IntPtr device, IntPtr devmode, int flags,
                                       uint access, IntPtr attributes);

    [DllImport("user32.dll", SetLastError = true)]
    static extern bool CloseDesktop(IntPtr desktop);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string app, System.Text.StringBuilder cmd, IntPtr pa, IntPtr ta,
                                     bool inherit, uint flags, IntPtr env, string dir,
                                     ref STARTUPINFO si, out PROCESS_INFORMATION pi);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr handle, uint ms);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeProcess(IntPtr process, out uint code);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr handle);

    const uint GENERIC_ALL = 0x10000000;
    const uint CREATE_NO_WINDOW = 0x08000000;

    public static int Run(string desktopName, string commandLine)
    {
        IntPtr desktop = CreateDesktop(desktopName, IntPtr.Zero, IntPtr.Zero, 0, GENERIC_ALL, IntPtr.Zero);
        if (desktop == IntPtr.Zero)
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "CreateDesktop");
        try {
            var si = new STARTUPINFO();
            si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            si.lpDesktop = "WinSta0\\" + desktopName;
            PROCESS_INFORMATION pi;
            var cmd = new System.Text.StringBuilder(
                Environment.ExpandEnvironmentVariables("%ComSpec%") + " /c \"" + commandLine + "\"");
            if (!CreateProcess(null, cmd, IntPtr.Zero, IntPtr.Zero, false, CREATE_NO_WINDOW,
                               IntPtr.Zero, null, ref si, out pi))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "CreateProcess");
            WaitForSingleObject(pi.hProcess, 0xFFFFFFFF);
            uint code;
            GetExitCodeProcess(pi.hProcess, out code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return (int)code;
        }
        finally {
            CloseDesktop(desktop);
        }
    }
}
'@

$code = [HiddenDesktop]::Run('fcad-gui-test', $CommandLine)
exit $code
