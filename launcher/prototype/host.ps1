# Gamebreaker launcher: a WinForms window holding Microsoft's WebView2
# control, showing ..\mockups\halo.html. The controller is read here with
# XInput and passed to the page, because the browser's own gamepad support is
# unreliable inside WebView2.
#
# Two modes, one page:
#   -Live    the real launcher. The page gets window.__gbLive and asks this
#            host to run the engine (..\..\setup\setup.ps1) and the game; the
#            engine's progress comes back as JSON lines (setup.ps1 -Events).
#            ..\..\Gamebreaker.cmd starts it this way.
#   default  the page alone with sample data, for working on its design.
#
# Public parameters: -Live, and -AfterPid <pid> (the launcher that waits for the
# game to end, then opens), with -AfterUpdate when that process is the launcher
# itself, restarting after an update replaced its files. Everything else is a test or design hook, kept
# between diagnostic markers so a public build leaves it out (publish\export.ps1).
param(
    [switch]$Live,
    [int]$AfterPid = 0,
    [switch]$AfterUpdate
)

$ErrorActionPreference = 'Stop'
$clock = [Diagnostics.Stopwatch]::StartNew()
$log = Join-Path $PSScriptRoot 'last-run.txt'
. (Join-Path $PSScriptRoot 'log_report.ps1')
# A launcher reopened after the game adds to the log of the one that started it.
if ($AfterPid -gt 0) { Add-Content -Path $log -Value ("reopened " + (Get-Date -Format s) + ", waiting for the game (pid $AfterPid)") -Encoding utf8 }
else { Set-Content -Path $log -Value ("started " + (Get-Date -Format s)) -Encoding utf8 }
function Note([string]$text) {
    $line = "{0,6} ms  {1}" -f $clock.ElapsedMilliseconds, $text
    # The launcher reopened after the game writes to the same file: a line written at the
    # same instant meets a sharing conflict, so try a few times before giving the line up.
    for ($i = 0; $i -lt 5; $i++) { try { Add-Content -Path $log -Value $line -Encoding utf8 -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 25 } }
    Write-Host $line
}
# Anything that stops the launcher before or outside its window is shown, not
# lost: there is no console to read it in. Exit code 3 tells Gamebreaker.exe the
# message was already shown.
function Show-Stop([string]$text) {
    try { Add-Type -AssemblyName System.Windows.Forms; [Windows.Forms.MessageBox]::Show($text, 'Gamebreaker', 'OK', 'Error') | Out-Null } catch { }
}
trap {
    Note ("stopped: " + $_.Exception.Message)
    Show-Stop ("The launcher could not start.`n`n" + $_.Exception.Message)
    exit 3
}

# Smart App Control, or another App Control policy that enforces scripts, runs
# unsigned scripts in Constrained Language Mode, where the window cannot be built
# (Add-Type is not allowed). Say so plainly instead of failing on the first Add-Type.
if ($ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage') {
    $sacOn = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState -eq 1
    Note ("language mode " + $ExecutionContext.SessionState.LanguageMode + "; Smart App Control on: $sacOn")
    # Plain words, no guide to Windows' settings (decided 2026-11-16).
    Show-Stop ($(if ($sacOn) { "Windows blocked the launcher from running on this PC.`n`nCheck Windows' security settings (Smart App Control), then try again." }
                else { "Windows blocked the launcher from running on this PC.`n`nCheck Windows' security settings, then try again." }))
    exit 3
}


# The game is running: wait for it to end (the launcher is closed while it runs,
# to free its memory), and remember how it ended for the page.
$script:lastGame = $null
if ($AfterPid -gt 0 -and $AfterUpdate) {
    # The launcher that installed an update: wait for it to close (it holds the
    # one-launcher lock). Nothing to report: it was not the game.
    try { $op = Get-Process -Id $AfterPid -ErrorAction Stop; $null = $op.Handle; $op.WaitForExit(30000) | Out-Null } catch { }
    Note 'restarted after an update'
} elseif ($AfterPid -gt 0) {
    try {
        $gp = Get-Process -Id $AfterPid -ErrorAction Stop
        $null = $gp.Handle
        $t0 = $gp.StartTime
        $gp.WaitForExit()
        $script:lastGame = @{ code = $gp.ExitCode; secs = [int]((Get-Date) - $t0).TotalSeconds }
        $script:lastGameStart = $t0
        Note ("game ended: exit code {0} after {1} s" -f $gp.ExitCode, $script:lastGame.secs)
    } catch { Note ("game already ended: " + $_.Exception.Message) }
}

# One live launcher at a time. A second start brings the open one to the front
# instead (the launcher that reopens after the game waits a few seconds for the
# one that started the game to finish closing).
$script:mutex = $null
if ($Live -and -not $TestFolder) {
    $script:mutex = New-Object Threading.Mutex($false, 'Local\Gamebreaker.Launcher')
    $got = $false
    try { $got = $script:mutex.WaitOne($(if ($AfterPid -gt 0) { 5000 } else { 0 })) } catch [Threading.AbandonedMutexException] { $got = $true }
    if (-not $got) {
        Note 'another launcher is open: bringing it to the front'
        Add-Type -Namespace GbOnce -Name W -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h); [DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int c); [DllImport("user32.dll")] public static extern bool IsIconic(System.IntPtr h);'
        Get-Process powershell -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $PID -and $_.MainWindowTitle -eq 'Gamebreaker' } | Select-Object -First 1 | ForEach-Object {
            if ([GbOnce.W]::IsIconic($_.MainWindowHandle)) { [GbOnce.W]::ShowWindow($_.MainWindowHandle, 9) | Out-Null }
            [GbOnce.W]::SetForegroundWindow($_.MainWindowHandle) | Out-Null
        }
        exit 0
    }
}

try { & (Join-Path $PSScriptRoot 'fetch-webview2.ps1') }
catch {
    Note ("WebView2 SDK not fetched: " + $_.Exception.Message)
    Show-Stop ("The launcher's window component (Microsoft's WebView2 SDK, 9 MB from nuget.org) could not be downloaded.`n`n" +
               $_.Exception.Message + "`n`nCheck the internet connection, then start Gamebreaker again.")
    exit 3
}

# What is remembered between runs: the page's settings (as the JSON text the
# page sent) and where the window was. One small file beside the script.
$statePath = Join-Path $PSScriptRoot 'state.json'
$script:pageState = $null
$savedWindow = $null
if (-not $script:pageState -and -not $NoState -and (Test-Path $statePath)) {
    try {
        $state = Get-Content $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($state.page) { $null = $state.page | ConvertFrom-Json; $script:pageState = [string]$state.page }
        $savedWindow = $state.window
    } catch { Note ("state.json ignored: " + $_.Exception.Message) }
}
function Save-State {
    if ($NoState) { return }
    $b = if ($form.WindowState -eq 'Normal') { $form.Bounds } else { $form.RestoreBounds }
    $window = [ordered]@{ x = $b.X; y = $b.Y; w = $b.Width; h = $b.Height; max = ($form.WindowState -eq 'Maximized') }
    if ($OffPrimary -or $Screen -ge 0) { $window = $savedWindow }   # placement was forced for a test: keep the remembered one
    $data = [ordered]@{ page = $script:pageState; window = $window }
    $tmp = "$statePath.tmp"
    Set-Content -Path $tmp -Value ($data | ConvertTo-Json -Depth 4) -Encoding UTF8
    Move-Item -Path $tmp -Destination $statePath -Force
}

if (-not $Page) { $Page = Join-Path $PSScriptRoot '..\mockups\halo.html' }
$Page = (Resolve-Path $Page).Path

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$lib = Join-Path $PSScriptRoot 'lib'
Add-Type -Path (Join-Path $lib 'Microsoft.Web.WebView2.Core.dll')
Add-Type -Path (Join-Path $lib 'Microsoft.Web.WebView2.WinForms.dll')

# Small native helpers. C# 5 syntax: Windows PowerShell compiles this itself.
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace GbHost {
    // A window with no title bar that still resizes, snaps and has a shadow.
    // WM_NCCALCSIZE: keep the side and bottom borders Windows gives a sizable
    // window, but take the caption's space back as client area.
    // WM_NCHITTEST: a thin band at the very top still resizes.
    public class LauncherForm : Form {
        public bool Quiet;
        public bool Frameless;
        public int Band = 4;
        [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
        [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
        [DllImport("user32.dll")] static extern bool IsZoomed(IntPtr hwnd);
        [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int w, int h, uint flags);
        protected override bool ShowWithoutActivation { get { return Quiet; } }
        public void FrameChanged() { SetWindowPos(Handle, IntPtr.Zero, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0004 | 0x0010 | 0x0020); }
        protected override void WndProc(ref Message m) {
            if (Frameless && m.Msg == 0x0083 && m.WParam != IntPtr.Zero) {
                RECT before = (RECT)Marshal.PtrToStructure(m.LParam, typeof(RECT));
                base.WndProc(ref m);
                RECT after = (RECT)Marshal.PtrToStructure(m.LParam, typeof(RECT));
                after.T = before.T;
                if (IsZoomed(Handle)) after.T += GetSystemMetrics(33) + GetSystemMetrics(92);
                Marshal.StructureToPtr(after, m.LParam, false);
                m.Result = IntPtr.Zero;
                return;
            }
            base.WndProc(ref m);
            if (Frameless && m.Msg == 0x0084 && (int)m.Result == 1 && !IsZoomed(Handle)) {
                int y = (short)(((long)m.LParam >> 16) & 0xFFFF);
                if (PointToClient(new System.Drawing.Point(0, y)).Y < Band) m.Result = (IntPtr)12;
            }
        }
    }

    // Monitors in EnumDisplayMonitors order: the order GB_D3D11_MONITOR=<n>
    // counts in (port\src\gb_d3d11.c), so the launcher's Display list and the
    // game agree on what "2" means.
    public static class Monitors {
        [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct MONITORINFOEX { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice; }
        delegate bool EnumProc(IntPtr h, IntPtr hdc, IntPtr rc, IntPtr data);
        [DllImport("user32.dll")] static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, EnumProc proc, IntPtr data);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool GetMonitorInfo(IntPtr h, ref MONITORINFOEX mi);
        public static string[] List() {
            var list = new System.Collections.Generic.List<string>();
            EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (h, hdc, rc, d) => {
                MONITORINFOEX mi = new MONITORINFOEX(); mi.cbSize = Marshal.SizeOf(typeof(MONITORINFOEX));
                if (GetMonitorInfo(h, ref mi)) list.Add((mi.rcMonitor.R - mi.rcMonitor.L) + "|" + (mi.rcMonitor.B - mi.rcMonitor.T) + "|" + ((mi.dwFlags & 1) != 0 ? 1 : 0) + "|" + mi.szDevice);
                return true; }, IntPtr.Zero);
            return list.ToArray();
        }
    }

    // The modern Explorer-style folder picker (IFileOpenDialog in folder mode).
    // WinForms' FolderBrowserDialog on .NET Framework is the old tree view.
    [ComImport, Guid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")] class FileOpenDialogCo { }
    [ComImport, Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItem {
        void BindToHandler(IntPtr pbc, ref Guid bhid, ref Guid riid, out IntPtr ppv);
        void GetParent(out IShellItem parent);
        void GetDisplayName(uint sigdn, [MarshalAs(UnmanagedType.LPWStr)] out string name);
        void GetAttributes(uint mask, out uint attribs);
        void Compare(IShellItem other, uint hint, out int order);
    }
    [ComImport, Guid("42f85136-db7e-439c-85f1-e4075d135fc8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IFileDialog {
        [PreserveSig] int Show(IntPtr parent);
        void SetFileTypes(uint count, IntPtr specs);
        void SetFileTypeIndex(uint i);
        void GetFileTypeIndex(out uint i);
        void Advise(IntPtr events, out uint cookie);
        void Unadvise(uint cookie);
        void SetOptions(uint fos);
        void GetOptions(out uint fos);
        void SetDefaultFolder(IShellItem item);
        void SetFolder(IShellItem item);
        void GetFolder(out IShellItem item);
        void GetCurrentSelection(out IShellItem item);
        void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string name);
        void GetFileName([MarshalAs(UnmanagedType.LPWStr)] out string name);
        void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string title);
        void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string label);
        void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string label);
        void GetResult(out IShellItem item);
    }
    public static class Pick {
        [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
        static extern void SHCreateItemFromParsingName(string path, IntPtr pbc, ref Guid riid, out IShellItem item);
        // Returns the chosen folder, or "" when cancelled.
        public static string Folder(IntPtr owner, string title, string start) {
            IFileDialog d = (IFileDialog)new FileOpenDialogCo();
            try {
                d.SetOptions(0x20 | 0x40 | 0x800);           // pick folders, file system only, path must exist
                d.SetTitle(title);
                d.SetOkButtonLabel("Select folder");
                string s = start;
                while (!string.IsNullOrEmpty(s) && !System.IO.Directory.Exists(s)) s = System.IO.Path.GetDirectoryName(s);
                if (!string.IsNullOrEmpty(s)) {
                    try { Guid iid = typeof(IShellItem).GUID; IShellItem si; SHCreateItemFromParsingName(s, IntPtr.Zero, ref iid, out si); d.SetFolder(si); } catch { }
                }
                if (d.Show(owner) != 0) return "";
                IShellItem r; d.GetResult(out r);
                string path; r.GetDisplayName(0x80058000, out path);  // SIGDN_FILESYSPATH
                return path ?? "";
            } finally { Marshal.ReleaseComObject(d); }
        }
    }

    public static class Native {
        [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
        [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
        // Tells the taskbar this is its own app, not another PowerShell window.
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)] public static extern int SetCurrentProcessExplicitAppUserModelID(string id);
        public static void DarkTitleBar(IntPtr hwnd) { int on = 1; DwmSetWindowAttribute(hwnd, 20, ref on, 4); }
    }

    // Reads the first connected XInput controller and turns it into launcher
    // events: directions (with key repeat), a, b, lb, rb, start, plus the raw
    // state for the controller test picture.
    public static class Pad {
        [StructLayout(LayoutKind.Sequential)] struct Gamepad { public ushort Buttons; public byte LT, RT; public short LX, LY, RX, RY; }
        [StructLayout(LayoutKind.Sequential)] struct State { public uint Packet; public Gamepad Pad; }
        [DllImport("xinput1_4.dll")] static extern int XInputGetState(int index, out State state);

        const int First = 380, Again = 110, Rescan = 1000;
        static int slot = -1;
        static long nextScan;
        static ushort prev;
        static long[] due = new long[4];
        static string lastRaw = "";

        public static string Poll(long now) {
            StringBuilder sb = new StringBuilder();
            State st = new State();
            bool ok = slot >= 0 && XInputGetState(slot, out st) == 0;
            if (!ok) {
                if (slot >= 0) { slot = -1; prev = 0; sb.Append("pad:0\n"); }
                if (now < nextScan) return sb.ToString();
                nextScan = now + Rescan;            // empty slots are slow to ask: do it once a second
                for (int i = 0; i < 4 && slot < 0; i++) if (XInputGetState(i, out st) == 0) slot = i;
                if (slot < 0) return sb.ToString();
                sb.Append("pad:1\n");
            }
            Gamepad g = st.Pad;
            const int T = 18000;
            bool[] dir = {
                (g.Buttons & 1) != 0 || g.LY > T, (g.Buttons & 2) != 0 || g.LY < -T,
                (g.Buttons & 4) != 0 || g.LX < -T, (g.Buttons & 8) != 0 || g.LX > T };
            string[] names = { "up", "down", "left", "right" };
            for (int i = 0; i < 4; i++) {
                if (!dir[i]) { due[i] = 0; continue; }
                if (due[i] == 0) { due[i] = now + First; sb.Append(names[i]).Append('\n'); }
                else if (now >= due[i]) { due[i] = now + Again; sb.Append(names[i]).Append('\n'); }
            }
            ushort pressed = (ushort)(g.Buttons & ~prev);
            prev = g.Buttons;
            if ((pressed & 0x1000) != 0) sb.Append("a\n");
            if ((pressed & 0x2000) != 0) sb.Append("b\n");
            if ((pressed & 0x4000) != 0) sb.Append("x\n");
            if ((pressed & 0x8000) != 0) sb.Append("y\n");
            if ((pressed & 0x0100) != 0) sb.Append("lb\n");
            if ((pressed & 0x0200) != 0) sb.Append("rb\n");
            if ((pressed & 0x0010) != 0) sb.Append("start\n");
            string raw = "s:" + g.Buttons.ToString("x") + ":" + (g.LX / 2048) + ":" + (g.LY / 2048) + ":" + (g.RX / 2048) + ":" + (g.RY / 2048) + ":" + (g.LT / 16) + ":" + (g.RT / 16);
            if (raw != lastRaw) { lastRaw = raw; sb.Append(raw).Append('\n'); }
            return sb.ToString();
        }
    }
}
'@

[GbHost.Native]::SetProcessDPIAware() | Out-Null
[GbHost.Native]::SetCurrentProcessExplicitAppUserModelID('Gamebreaker.Launcher') | Out-Null

# Which screen
$screens = [Windows.Forms.Screen]::AllScreens
$target = [Windows.Forms.Screen]::PrimaryScreen
$area = $target.WorkingArea
# The window's first size: a 16:9 shape filling this share of the screen's work area.
function Get-OpeningSize($area, [double]$share) {
    $cw = [int]([Math]::Min($area.Width * $share, $area.Height * $share * 16 / 9))
    New-Object Drawing.Size($cw, [int]($cw * 9 / 16))
}
$opening = Get-OpeningSize $area 0.56

$dark = [Drawing.Color]::FromArgb(7, 8, 13)
$form = New-Object GbHost.LauncherForm
$form.Quiet = [bool]$Quiet
$form.Frameless = -not $Framed
$form.Text = 'Gamebreaker'
$iconPath = Join-Path $PSScriptRoot 'gamebreaker.ico'
if (Test-Path $iconPath) { $form.Icon = New-Object Drawing.Icon($iconPath) }
$form.BackColor = $dark
$form.StartPosition = 'Manual'
$form.MinimumSize = New-Object Drawing.Size(960, 600)
# Every close remembers the window's size, so a remembered size equal to the earlier
# opening size (70% of the work area) was never chosen: it gives way to the new one.
$form.ClientSize = Get-OpeningSize $area 0.7
$earlierSize = $form.Size
$form.ClientSize = $opening
$form.Location = New-Object Drawing.Point(($area.X + ($area.Width - $form.Width) / 2), ($area.Y + ($area.Height - $form.Height) / 2))
if ($savedWindow -and $Screen -lt 0 -and -not $OffPrimary) {
    $r = New-Object Drawing.Rectangle([int]$savedWindow.x, [int]$savedWindow.y, [int]$savedWindow.w, [int]$savedWindow.h)
    $visible = $screens | Where-Object { $i = [Drawing.Rectangle]::Intersect($_.WorkingArea, $r); $i.Width -ge 200 -and $i.Height -ge 120 }
    if ($visible -and $r.Width -ge 960 -and $r.Height -ge 600) {
        if ($r.Size -eq $earlierSize) {
            # Stays where the player left it, centred on the same spot.
            $form.Location = New-Object Drawing.Point(($r.X + ($r.Width - $form.Width) / 2), ($r.Y + ($r.Height - $form.Height) / 2))
        } else { $form.Bounds = $r }
        if ($savedWindow.max) { $form.WindowState = 'Maximized' }
    }
}
$form.add_FormClosing({ try { Save-State } catch { Note ('state not saved: ' + $_.Exception.Message) } })
$form.add_HandleCreated({ [GbHost.Native]::DarkTitleBar($form.Handle); if ($form.Frameless) { $form.FrameChanged() } })
# The top band is the form itself showing through, so its edge can resize the window.
function Set-Band { if ($form.Frameless -and $form.WindowState -eq 'Normal') { $form.Padding = New-Object Windows.Forms.Padding(0, $form.Band, 0, 0) } else { $form.Padding = New-Object Windows.Forms.Padding(0) } }
Set-Band
$form.add_Resize({
    Set-Band
    if ($script:ready -and $form.WindowState -ne 'Minimized') {
        $view.CoreWebView2.PostWebMessageAsString($(if ($form.WindowState -eq 'Maximized') { 'win:max' } else { 'win:normal' }))
    }
})

$view = New-Object Microsoft.Web.WebView2.WinForms.WebView2
$view.Dock = 'Fill'
$view.DefaultBackgroundColor = $dark
$props = New-Object Microsoft.Web.WebView2.WinForms.CoreWebView2CreationProperties
$props.UserDataFolder = Join-Path $PSScriptRoot '.webview'
$props.AdditionalBrowserArguments = '--autoplay-policy=no-user-gesture-required'
$view.CreationProperties = $props

$script:ready = $false
$view.add_CoreWebView2InitializationCompleted({
    param($sender, $e)
    if (-not $e.IsSuccess) {
        Note ("WebView2 failed to start: " + $e.InitializationException.Message)
        $form.Hide()
        Show-Stop ("The launcher's window could not start: " + $e.InitializationException.Message + "`n`n" +
                   "It needs the Microsoft Edge WebView2 Runtime, which is part of Windows 11. On Windows 10, install the " +
                   "Evergreen WebView2 Runtime from Microsoft's website, then start Gamebreaker again.")
        $script:exitCode = 3
        $form.Close()
        return
    }
    Note ("WebView2 runtime " + $view.CoreWebView2.Environment.BrowserVersionString)
    $s = $view.CoreWebView2.Settings
    $s.AreDefaultContextMenusEnabled = $false
    $s.IsStatusBarEnabled = $false
    $s.IsZoomControlEnabled = $false
    $s.AreBrowserAcceleratorKeysEnabled = [bool]$Dev
    $s.AreDevToolsEnabled = [bool]$Dev
    if ($Mute) { try { $view.CoreWebView2.IsMuted = $true; Note 'page muted' } catch { Note 'this WebView2 cannot mute the page' } }
    if ($Live) { $view.CoreWebView2.AddScriptToExecuteOnDocumentCreatedAsync('window.__gbLive = ' + (Get-LiveInfo) + ';') | Out-Null }
    try { $s.IsNonClientRegionSupportEnabled = $true } catch { Note 'this WebView2 SDK cannot let the page drag the window' }
    # Script errors in the page are reported to the host log.
    $view.CoreWebView2.AddScriptToExecuteOnDocumentCreatedAsync("addEventListener('error', function (e) { try { chrome.webview.postMessage('script error: ' + e.message + ' (line ' + e.lineno + ')'); } catch (x) {} });") | Out-Null
    if ($script:pageState) { $view.CoreWebView2.AddScriptToExecuteOnDocumentCreatedAsync('window.__gbSaved = ' + $script:pageState + ';') | Out-Null }
    $view.CoreWebView2.add_WebMessageReceived({
        param($s2, $m)
        $text = $m.TryGetWebMessageAsString()
        if ($text.StartsWith('save:')) {
            $json = $text.Substring(5)
            if ($json.Length -lt 20000) { try { $null = $json | ConvertFrom-Json; $script:pageState = $json; Save-State } catch { Note 'page sent settings that are not valid JSON; ignored' } }
            return
        }
        # A file dropped on the page arrives as an object beside the message (WebView2's
        # postMessageWithAdditionalObjects); its path is read here, never sent by the page.
        if ($text -eq 'live:modsdrop') {
            $dp = ''
            try { foreach ($ob in $m.AdditionalObjects) { if ($ob -and $ob.Path) { $dp = [string]$ob.Path; break } } } catch { Note ('drop: ' + $_.Exception.Message) }
            if ($dp) { Send-ModInspect $dp } else { Send-EngObj ([ordered]@{ ev = 'modinspect'; ok = $false; zip = ''; reason = 'The dropped file could not be read.' }) }
            return
        }
        if ($text.StartsWith('live:')) { try { Invoke-Live $text.Substring(5) } catch { Note ('live: ' + $_.Exception.Message); Send-Eng @{ ev = 'error'; step = 'launcher'; code = 'launcher'; text = $_.Exception.Message } }; return }
        Note ("page says: " + $text)
        switch ($text) {
            'win:min'   { $form.WindowState = 'Minimized' }
            'win:max'   { if ($form.WindowState -eq 'Maximized') { $form.WindowState = 'Normal' } else { $form.WindowState = 'Maximized' } }
            'win:close' { $form.Close() }
        }
    })
})
$view.add_NavigationCompleted({
    param($sender, $e)
    if ($script:ready) { return }
    $script:ready = $true
    Note ("page shown (success=" + $e.IsSuccess + ")")
})
$form.Controls.Add($view)

# ------------------------------------------------------------------ live mode
# Everything the real launcher does besides showing the page. The page sends
# 'live:<what>'; answers go back as 'eng:<json>' lines, the same shape as the
# engine's own events (setup.ps1 -Events), which are forwarded unchanged.
$script:relRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path     # the release (or repository) folder
$script:setupPs = Join-Path $script:relRoot 'setup\setup.ps1'
$script:updatePs = Join-Path $script:relRoot 'setup\update.ps1'
$script:upDir = Join-Path $script:relRoot 'update'
$script:folderFile = Join-Path $script:relRoot 'setup\folder.txt'
$relManifest = Join-Path $script:relRoot 'setup\release_manifest.ps1'
if (Test-Path -LiteralPath $relManifest) { . $relManifest }
# The install folder for a folder the player picked: inside the release, app\game, so the
# folder holding Gamebreaker.exe stays clean; elsewhere a subfolder named Gamebreaker.
function Get-InstallChoice([string]$picked) {
    $full = [IO.Path]::GetFullPath($picked)
    if (Get-Command Resolve-GbInstallFolder -ErrorAction SilentlyContinue) {
        $inside = Resolve-GbInstallFolder $script:relRoot $full
        if ($inside -ne $full) { return $inside }
    }
    if ((Split-Path $full -Leaf) -ne 'Gamebreaker' -and -not (Test-Path (Join-Path $full 'state'))) { return (Join-Path $full 'Gamebreaker') }
    $full
}
$script:homeDefault = Join-Path $env:LOCALAPPDATA 'Gamebreaker'
if ($DefaultFolder) { $script:homeDefault = $DefaultFolder }   # tests on a PC that already has an install there
$script:folder = $script:homeDefault
if ((Test-Path $script:folderFile) -and -not $DefaultFolder) { $f = (Get-Content $script:folderFile -Raw).Trim(); if ($f) { $script:folder = $f } }
if ($TestFolder) { $script:folder = $TestFolder }      # tests: a scratch install, never remembered
$script:disc = ''
$script:eng = $null; $script:engKind = ''; $script:evFile = ''; $script:evPos = 0; $script:evRest = ''; $script:runId = 0

function Send-Eng([hashtable]$o) { if ($script:ready) { $view.CoreWebView2.PostWebMessageAsString('eng:' + ($o | ConvertTo-Json -Compress -Depth 4)) } }
function Send-EngObj($o) { if ($script:ready) { $view.CoreWebView2.PostWebMessageAsString('eng:' + ($o | ConvertTo-Json -Compress -Depth 6)) } }

# Mods (setup\mods_lib.ps1). The folder is <install>\mods, beside saves\, where
# the game looks (port\src\gb_mods.c); the launcher writes there only the mod
# folders it installs or removes and mods\load_order.txt. A zip is looked at
# here and unpacked by setup\mods.ps1 in its own process.
. (Join-Path $script:relRoot 'setup\mods_lib.ps1')
function Get-ModsDir { Join-Path $script:folder 'mods' }
function Get-GameFilesDir { Join-Path $script:folder 'game_files' }
$script:modZip = ''; $script:modToken = ''
function Send-ModList {
    $md = Get-ModsDir
    try { Clear-GbModTemp $md; Send-EngObj ([ordered]@{ ev = 'mods'; list = (Get-GbModList $md (Get-GameFilesDir) (Join-Path $script:folder 'logs\game.log')).ToArray() }) }
    catch { Note ('mods not listed: ' + $_.Exception.Message); Send-EngObj ([ordered]@{ ev = 'mods'; list = @(); error = 'The mods folder could not be read.' }) }
}
function Send-ModInspect([string]$path) {
    $script:modZip = ''; $script:modToken = ''
    try { $i = Get-GbZipInfo $path (Get-ModsDir) (Get-GameFilesDir) }
    catch { Note ('mod zip not read: ' + $_.Exception.Message); $i = [ordered]@{ ok = $false; zip = (Split-Path $path -Leaf); reason = 'This file could not be read.' } }
    if ($i.ok) { $script:modZip = $path; $script:modToken = [guid]::NewGuid().ToString('N'); $i.token = $script:modToken }
    $i.ev = 'modinspect'
    Note ('mod zip looked at: ' + $i.zip + $(if ($i.ok) { ' (' + $i.folder + ', ' + @($i.files).Count + ' game files)' } else { ': ' + $i.reason }))
    Send-EngObj $i
}
function Test-Installed([string]$f) {
    (Test-Path (Join-Path $f 'build\gamebreaker.exe')) -and (Test-Path (Join-Path $f 'state\lift.done')) -and (Test-Path (Join-Path $f 'game_files\default.xbe'))
}
# The pinned tools (setup\tools.lock) and whether each is already in place in
# the install folder, so the page can show sizes and what is left to fetch.
function Get-ToolList {
    $lock = Join-Path $script:relRoot 'setup\tools.lock'
    @((Get-Content $lock -Raw | ConvertFrom-Json).tools | ForEach-Object {
        $dir = Join-Path $script:folder ("tools\" + $_.dir); $m = Join-Path $dir '.gb_tool'
        $pin = $(if ($_.verify -eq 'tree') { $_.tree_sha256 } else { $_.sha256 })
        $here = ((Test-Path (Join-Path $dir $_.check)) -and (Test-Path $m) -and ((Get-Content $m -Raw).Trim() -eq $pin))
        # A toolkit shipped beside setup is used instead of a download (setup checks its files).
        if ($_.name -eq 'xboxrecomp' -and (Test-Path (Join-Path $script:relRoot 'xboxrecomp\LICENSE'))) { $here = $true }
        @{ name = $_.name; size = $_.size; present = $here }
    })
}
# Graphics adapters, for the page's defaults (resolution, anti-aliasing and
# texture filtering are lighter when the PC has integrated graphics only:
# such a chip has a fraction of a card's pixel rate). The names are read by
# setup\gpu_names.ps1, the same file setup's "This PC" line uses.
. (Join-Path $script:relRoot 'setup\gpu_names.ps1')
function Get-GraphicsInfo {
    $names = @()
    if (-not $names) {
        try {
            $names = @(Get-UsableGpu @(Get-CimInstance Win32_VideoController -ErrorAction Stop) | ForEach-Object { [string]$_.Name })
        } catch { $names = @() }
    }
    $list = @($names | ForEach-Object { @{ name = $_; integrated = [bool](Test-IntegratedGpu $_) } })
    @{ adapters = $list; integratedOnly = ($list.Count -gt 0 -and -not ($list | Where-Object { -not $_.integrated })) }
}
function Get-LiveInfo {
    $ver = 'dev'
    $rel = Join-Path $script:relRoot 'setup\release.txt'
    if (Test-Path $rel) { $m = Select-String -Path $rel -Pattern '^version=(.+)$' | Select-Object -First 1; if ($m) { $ver = $m.Matches[0].Groups[1].Value.Trim() } }
    $mons = @([GbHost.Monitors]::List() | ForEach-Object { $a = $_ -split '\|'; @{ w = [int]$a[0]; h = [int]$a[1]; primary = ($a[2] -eq '1') } })
    # A fatal error in the game ends it with exit code 0 (the toolkit's
    # ExitProcess(0)); its own log says so (port\src\gb_save.c, gb_fatal_report).
    if ($script:lastGame -and $script:lastGameStart) {
        foreach ($lf in 'logs\game.log', 'logs\game_stderr.txt') {
            $p = Join-Path $script:folder $lf
            if (-not (Test-Path $p) -or (Get-Item $p).LastWriteTime -lt $script:lastGameStart) { continue }
            $m = Select-String -Path $p -Pattern '\[GB\] the game stopped on its own fatal error: (.*)$' | Select-Object -Last 1
            if ($m) { $script:lastGame.fatal = $m.Matches[0].Groups[1].Value.Trim(); Note ("the game stopped on a fatal error: " + $script:lastGame.fatal); break }
        }
    }
    $info = [ordered]@{
        folder = $script:folder; defaultFolder = $script:homeDefault
        installed = (Test-Installed $script:folder); version = $ver; displays = $mons; gpu = (Get-GraphicsInfo); tools = (Get-ToolList)
        lastGame = $script:lastGame; auto = $Auto; dev = [bool]$Dev; update = (Get-UpdateInfo)
    }
    $info | ConvertTo-Json -Compress -Depth 4
}

# Run setup.ps1 hidden, following its events file.
function Start-Engine([string]$kind, [string[]]$engArgs, [string]$engPs = '') {
    if ($script:eng -and -not $script:eng.HasExited) { Send-Eng @{ ev = 'error'; step = 'launcher'; code = 'busy'; text = 'Setup is already running.' }; return }
    $script:runId++
    $script:evFile = Join-Path ([IO.Path]::GetTempPath()) ("gb_launcher_{0}_{1}.jsonl" -f $PID, $script:runId)
    Set-Content -Path $script:evFile -Value '' -NoNewline -Encoding ascii
    $script:evPos = 0; $script:evRest = ''; $script:engKind = $kind; $script:lastEngError = ''
    if (-not $engPs) { $engPs = $script:setupPs }
    $q = { param($a) if ($a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a } }
    $list = @('-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (& $q $engPs)) + @($engArgs | ForEach-Object { & $q $_ }) +
            @('-NoPrompt', '-Events', (& $q $script:evFile))
    $out = Join-Path ([IO.Path]::GetTempPath()) ("gb_launcher_{0}_{1}.out.txt" -f $PID, $script:runId)
    $script:eng = Start-Process -FilePath 'powershell.exe' -ArgumentList ($list -join ' ') -WindowStyle Hidden -PassThru `
                    -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    $null = $script:eng.Handle
    Note ("engine started ($kind): " + ($engArgs -join ' '))
    $engTimer.Start()
}
# The update check runs beside the engine, never in its place: a slow or
# unreachable feed must not hold up setup, Rebuild, Check files or Move.
$script:chk = $null; $script:chkFile = ''; $script:chkPos = 0; $script:chkRest = ''
function Start-UpdateCheck {
    if ($script:chk -and -not $script:chk.HasExited) { return }
    $script:chkFile = Join-Path ([IO.Path]::GetTempPath()) ("gb_launcher_{0}_check.jsonl" -f $PID)
    Set-Content -Path $script:chkFile -Value '' -NoNewline -Encoding ascii
    $script:chkPos = 0; $script:chkRest = ''
    $a = '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "{0}" -Action check -Events "{1}"' -f $script:updatePs, $script:chkFile
    $script:chk = Start-Process -FilePath 'powershell.exe' -ArgumentList $a -WindowStyle Hidden -PassThru
    $null = $script:chk.Handle
    Note 'update check started'
    $chkTimer.Start()
}
$chkTimer = New-Object Windows.Forms.Timer
$chkTimer.Interval = 250
$chkTimer.add_Tick({
    $exited = $script:chk.HasExited
    try {
        $fs = [IO.File]::Open($script:chkFile, 'Open', 'Read', 'ReadWrite')
        try {
            if ($fs.Length -gt $script:chkPos) {
                $fs.Position = $script:chkPos
                $buf = New-Object byte[] ($fs.Length - $script:chkPos)
                $n = $fs.Read($buf, 0, $buf.Length); $script:chkPos += $n
                $lines = ($script:chkRest + [Text.Encoding]::UTF8.GetString($buf, 0, $n)) -split "`n"
                $script:chkRest = $lines[-1]
                foreach ($l in $lines[0..($lines.Count - 2)]) { if ($l.Trim()) { $view.CoreWebView2.PostWebMessageAsString('eng:' + $l.Trim()) } }
            }
        } finally { $fs.Dispose() }
    } catch { }
    if ($exited) { $chkTimer.Stop(); Note ("update check ended: exit code " + $script:chk.ExitCode); Remove-Item $script:chkFile -ErrorAction SilentlyContinue }
})
$engTimer = New-Object Windows.Forms.Timer
$engTimer.Interval = 150
$engTimer.add_Tick({
    $exited = $script:eng.HasExited
    try {
        $fs = [IO.File]::Open($script:evFile, 'Open', 'Read', 'ReadWrite')
        try {
            if ($fs.Length -gt $script:evPos) {
                $fs.Position = $script:evPos
                $buf = New-Object byte[] ($fs.Length - $script:evPos)
                $n = $fs.Read($buf, 0, $buf.Length); $script:evPos += $n
                $text = $script:evRest + [Text.Encoding]::UTF8.GetString($buf, 0, $n)
                $lines = $text -split "`n"
                $script:evRest = $lines[-1]
                foreach ($l in $lines[0..($lines.Count - 2)]) {
                    if (-not $l.Trim()) { continue }
                    if ($l -match '"ev":"error"') { try { $script:lastEngError = [string]($l | ConvertFrom-Json).text } catch { } }
                    $view.CoreWebView2.PostWebMessageAsString('eng:' + $l.Trim())
                }
            }
        } finally { $fs.Dispose() }
    } catch { }
    if ($exited) {
        $engTimer.Stop()
        $code = $script:eng.ExitCode
        Note ("engine ended ($($script:engKind)): exit code $code")
        Send-Eng @{ ev = 'exit'; kind = $script:engKind; code = $code; installed = (Test-Installed $script:folder) }
        if ($code -ne 0 -and $script:lastEngError) { Note ("engine error: " + $script:lastEngError) }
        Step-Update $script:engKind $code
    }
})

# Updates (setup\update.ps1). The chain: upinstall (download, check, replace the
# files) -> the launcher restarts itself, so the new release's launcher and
# setup do the rest -> upbuild (setup.ps1: lifts and compiles again only what
# changed) -> upfinish. A failure after files were replaced -> uproll (the old
# files and game program come back), and a restart if launcher files changed.
# The feed is a setting in setup\release.txt (feed=); none in a checkout.
function Get-UpdateJournal { $jf = Join-Path $script:upDir 'journal.json'; if (Test-Path $jf) { try { Get-Content $jf -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $null } } }
function Set-JournalShown {
    $jf = Join-Path $script:upDir 'journal.json'
    $j = Get-UpdateJournal
    if ($j -and -not $j.shown) { $j.shown = $true; [IO.File]::WriteAllText($jf, ($j | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding($false))) }
}
function Get-UpdateInfo {
    $rel = Join-Path $script:relRoot 'setup\release.txt'
    $feed = ''
    if ((Test-Path $rel) -and -not (Test-Path (Join-Path $script:relRoot '.git'))) {
        $m = Select-String -Path $rel -Pattern '^feed=(.+)$' | Select-Object -First 1; if ($m) { $feed = $m.Matches[0].Groups[1].Value.Trim() }
    }
    $due = $true
    $lc = Join-Path $script:upDir 'last-check.txt'
    if (Test-Path $lc) { try { $due = ((Get-Date).ToUniversalTime() - [datetime]::Parse((Get-Content $lc -Raw).Trim()).ToUniversalTime()).TotalHours -ge 20 } catch { } }
    $notes = @()
    $own = Join-Path $script:relRoot 'setup\manifest.json'
    if (Test-Path $own) { try { $notes = @((Get-Content $own -Raw -Encoding UTF8 | ConvertFrom-Json).notes) } catch { } }
    $j = Get-UpdateJournal
    $jo = $null
    if ($j) { $jo = @{ state = [string]$j.state; from = [string]$j.from; to = [string]$j.to; reason = [string]$j.reason; shown = [bool]$j.shown; launcher = [bool]$j.launcher; rebuild = [string]$j.rebuild } }
    # A finished or failed update is reported once.
    if ($j -and $j.state -in 'done', 'rolledback' -and -not $j.shown) { Set-JournalShown }
    @{ feed = [bool]$feed; due = $due; notes = $notes; journal = $jo }
}
# The command line that opens a new launcher once process $waitPid has ended.
function Get-RelaunchArgs([int]$waitPid, [switch]$Update) {
    $again = @('--headless', 'powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'), '-Live', '-AfterPid', $waitPid)
    if ($Update) { $again += '-AfterUpdate' }
    $again
}
function Restart-Launcher {
    Note 'restarting the launcher after an update'
    Start-Process -FilePath 'conhost.exe' -ArgumentList (Get-RelaunchArgs $PID -Update)
    $closeTimer.Start()
}
function Step-Update([string]$kind, [int]$code) {
    switch ($kind) {
        'upinstall' {
            if ($code -eq 0) { Restart-Launcher; return }
            $j = Get-UpdateJournal
            if ($j -and $j.state -in 'applying', 'applied') {
                Start-Engine 'uproll' @('-Action', 'rollback', '-Folder', $script:folder, '-Reason', ("The files could not all be replaced. " + $script:lastEngError).Trim()) $script:updatePs
            }
        }
        'upbuild' {
            if ($code -eq 0) { Start-Engine 'upfinish' @('-Action', 'finish') $script:updatePs; return }
            $why = $(if ($script:lastEngError) { "The rebuild stopped: $($script:lastEngError)" } else { "The rebuild stopped (exit code $code)." })
            Start-Engine 'uproll' @('-Action', 'rollback', '-Folder', $script:folder, '-Reason', $why) $script:updatePs
        }
        'uproll' {
            $j = Get-UpdateJournal
            # Launcher files came back: open the old launcher, which reports the failure.
            if ($code -eq 0 -and $j -and $j.launcher) { Restart-Launcher } else { Set-JournalShown }
        }
        'upfinish' { Set-JournalShown }
        'move' {
            # The game is in its new folder: remember it there (setup and the next start use it).
            if ($code -eq 0 -and $script:moveTo) {
                $script:folder = $script:moveTo
                if (-not $TestFolder) { try { Set-Content -Path $script:folderFile -Value $script:folder -Encoding utf8 } catch { } }
                Note "install folder moved to $($script:folder)"
                Send-Eng @{ ev = 'folder'; path = $script:folder; changed = $false; installed = (Test-Installed $script:folder); tools = (Get-ToolList) }
            }
            $script:moveTo = ''
        }
    }
}

function Select-Path([string]$kind) {
    $script:inDialog = $true
    try { Select-PathInner $kind } finally { $script:inDialog = $false }
}
function Select-PathInner([string]$kind) {
    if ($kind -eq 'modzip') {
        if ($TestModZip) { return $TestModZip }
        $d = New-Object Windows.Forms.OpenFileDialog
        $d.Title = 'Install a mod'; $d.Filter = 'Mod (*.zip)|*.zip|All files (*.*)|*.*'
        if ($d.ShowDialog($form) -eq 'OK') { return $d.FileName }
        return ''
    }
    if ($kind -eq 'file') {
        if ($TestDisc) { return $TestDisc }
        $d = New-Object Windows.Forms.OpenFileDialog
        $d.Title = 'Disc image'; $d.Filter = 'Xbox disc image (*.iso;*.xiso)|*.iso;*.xiso|All files (*.*)|*.*'
        if ($d.ShowDialog($form) -eq 'OK') { return $d.FileName }
    } else {
        if ($kind -eq 'disc' -and $TestDisc) { return $TestDisc }
        if ($kind -eq 'install' -and $TestFolder) { return $TestFolder }
        $title = $(if ($kind -eq 'install') { 'Install folder' } elseif ($kind -eq 'move') { 'Move the game to' } else { 'Folder with the extracted disc' })
        $startAt = $(if ($kind -eq 'install') { $script:folder } elseif ($kind -eq 'move') { Split-Path $script:folder } else { '' })
        return [GbHost.Pick]::Folder($form.Handle, $title, $startAt)
    }
    return ''
}

# The launcher's settings as the game's switches (port\src\gb_d3d11.c,
# recomp_manual.c). Anything not set here is left to the program's defaults.
# The player's own controls.ini (in the install folder, never the release's
# default): sets "rumble = N" in [player1] to [player4] and keeps every other
# line, so remappings a player wrote by hand survive.
function Set-PadRumble([string]$path, [int[]]$values) {
    $lines = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $path) { foreach ($l in [IO.File]::ReadAllLines($path)) { $lines.Add($l) } }
    else {
        $lines.Add('; controls.ini - your controller settings for Gamebreaker.')
        $lines.Add('; The launcher (Settings) writes the rumble and keyboard lines; anything else you add is kept.')
        $lines.Add('; Every key is described in the release''s port\config\controls.ini.')
    }
    for ($pl = 1; $pl -le 4; $pl++) {
        $line = "rumble = $($values[$pl - 1])"
        # Every [playerN] section (a hand-written file may repeat one), so no later line wins over this one.
        $first = -1; $set1 = $false; $inSec = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '^\s*\[') { $inSec = $lines[$i] -match "^\s*\[player$pl\]\s*(;.*)?$"; if ($inSec -and $first -lt 0) { $first = $i }; continue }
            if ($inSec -and $lines[$i] -match '^\s*rumble\s*=') { $lines[$i] = $line; $set1 = $true }
        }
        if ($first -lt 0) { $lines.Add(''); $lines.Add("[player$pl]"); $lines.Add($line) }
        elseif (-not $set1) { $lines.Insert($first + 1, $line) }
    }
    $tmp = "$path.tmp"
    [IO.File]::WriteAllLines($tmp, $lines, (New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $tmp $path -Force
}
# The keyboard's keys, as key_<control> lines in [player1] to [player4] of the
# same file. Only keys that differ from the defaults are in it.
$script:keyControls = @('a', 'b', 'x', 'y', 'black', 'white', 'left_trigger', 'right_trigger', 'start', 'back', 'left_thumb', 'right_thumb',
    'dpad_up', 'dpad_down', 'dpad_left', 'dpad_right', 'left_stick_up', 'left_stick_down', 'left_stick_left', 'left_stick_right',
    'right_stick_up', 'right_stick_down', 'right_stick_left', 'right_stick_right')
function Get-PadKeys([string]$path) {
    $out = @{ '1' = @{}; '2' = @{}; '3' = @{}; '4' = @{} }
    if (-not (Test-Path -LiteralPath $path)) { return $out }
    $pl = ''
    foreach ($l in [IO.File]::ReadAllLines($path)) {
        if ($l -match '^\s*\[') { $pl = $(if ($l -match '^\s*\[player([1-4])\]') { $Matches[1] } else { '' }); continue }
        if ($pl -and $l -match '^\s*key_([a-z_]+)\s*=\s*([^;#]*)') {
            $ctl = $Matches[1].ToLowerInvariant(); $v = $Matches[2].Trim()
            if ($script:keyControls -contains $ctl) { $out[$pl][$ctl] = $v }
        }
    }
    $out
}
function Set-PadKeys([string]$path, [int]$player, $keys) {
    if ($player -lt 1 -or $player -gt 4) { throw "no player $player" }
    $new = New-Object System.Collections.Generic.List[string]
    foreach ($p in $keys.PSObject.Properties) {
        if ($script:keyControls -notcontains $p.Name) { throw "no control $($p.Name)" }
        if ([string]$p.Value -notmatch '^[A-Za-z0-9]+(\s*,\s*[A-Za-z0-9]+){0,3}$') { throw "not a key: $($p.Value)" }
        $new.Add("key_$($p.Name) = $($p.Value)")
    }
    $lines = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $path) { foreach ($l in [IO.File]::ReadAllLines($path)) { $lines.Add($l) } }
    else {
        New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
        $lines.Add('; controls.ini - your controller settings for Gamebreaker.')
        $lines.Add('; The launcher (Settings) writes the rumble and keyboard lines; anything else you add is kept.')
        $lines.Add('; Every key is described in the release''s port\config\controls.ini.')
    }
    # Every [playerN] section loses its key lines; the first one takes the new ones.
    $first = -1; $inSec = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[') { $inSec = $lines[$i] -match "^\s*\[player$player\]\s*(;.*)?$"; if ($inSec -and $first -lt 0) { $first = $i }; continue }
        if ($inSec -and $lines[$i] -match '^\s*key_[A-Za-z_]+\s*=') { $lines.RemoveAt($i); $i-- }
    }
    if ($new.Count) {
        if ($first -lt 0) { $lines.Add(''); $lines.Add("[player$player]"); $first = $lines.Count - 1 }
        $lines.InsertRange($first + 1, $new)
    }
    $tmp = "$path.tmp"
    [IO.File]::WriteAllLines($tmp, $lines, (New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $tmp $path -Force
}
function Send-PadKeys { Send-Eng @{ ev = 'keys'; ok = $true; players = (Get-PadKeys (Join-Path $script:folder 'controls.ini')) } }
function Get-GameEnv($set) {
    $ge = [ordered]@{}
    if ($set.windowMode -eq 'fullscreen') { $ge.GB_D3D11_FULLSCREEN = '1' }
    if ([int]$set.monitor -gt 0) { $ge.GB_D3D11_MONITOR = [string][int]$set.monitor }      # 0 = the primary; n = Windows' n-th monitor
    if ($set.res -eq 'fit') { $ge.GB_D3D11_RES = 'window' } elseif ($set.res) { $ge.GB_D3D11_RES = [string]$set.res }
    $ge.GB_D3D11_VSYNC = $(if ($set.vsync) { 'auto' } else { '0' })
    # Widescreen: more of the court at the sides, menus and HUD kept 4:3 in the middle.
    if ($set.aspect -and $set.aspect -ne '4:3') {
        $ge.GB_ASPECT = [string]$set.aspect
        # Screens drawn flat: 'stretch' (the default: all 2D stretched), 'auto'
        # (menus, title and films stretched, the HUD over the court kept 4:3),
        # or 4:3 (all 2D in the middle, bars at the sides).
        if ($set.aspect2d -eq 'auto' -or $set.aspect2d -eq 'stretch') { $ge.GB_ASPECT_2D = [string]$set.aspect2d }
    }
    # Renderer quality (2026-11-16): texture filtering 0 (off), 1 (trilinear)
    # or anisotropic 2 to 16; multisampling 0, 2, 4, 8.
    if ($set.aniso -match '^(0|1|2|4|8|16)$') { $ge.GB_D3D11_ANISO = [string]$set.aniso }
    if ($set.msaa -match '^(0|2|4|8)$') { $ge.GB_D3D11_MSAA = [string]$set.msaa }
    # Graphics chip: unset is Windows' choice (the game asks for the dedicated
    # chip itself); 'high' asks DXGI for the high-performance one.
    if ($set.gpuPref -eq 'high') { $ge.GB_D3D11_ADAPTER = 'high' }
    # Four-player controllers (port\src\gb_pad.c, 2026-11-16): GB_PADS=1 is the
    # master switch; the others have no effect without it. Off by default until
    # it has been tested with real controllers.
    if ($set.pads) {
        $ge.GB_PADS = '1'
        if ($set.preset -in 'original', 'turbo') { $ge.GB_PAD_PRESET = [string]$set.preset }
        $ge.GB_PAD_TRIGGER = [string][Math]::Max(0, [Math]::Min(95, [int]$set.trigger))
        $ge.GB_PAD_DEADZONE = [string][Math]::Max(0, [Math]::Min(95, [int]$set.deadzone))
        # Rumble per player: the player's own <install>\controls.ini carries one
        # rumble line per [playerN] (everything else in that file is kept), so
        # GB_PAD_RUMBLE, which would set every player alike, is not passed.
        $rum = @(1..4 | ForEach-Object { $v = $set."rumble$_"; if ($null -eq $v) { $v = $(if ($null -ne $set.rumble) { $set.rumble } else { 100 }) }; [Math]::Max(0, [Math]::Min(100, [int]$v)) })
        $ini = Join-Path $script:folder 'controls.ini'
        try { Set-PadRumble $ini $rum; $ge.GB_PAD_CONFIG = $ini }
        catch { Note ("controls.ini not written: " + $_.Exception.Message); $ge.GB_PAD_RUMBLE = [string]$rum[0] }
        if ($set.padOrder -in 'auto', 'slots') { $ge.GB_PAD_ORDER = [string]$set.padOrder }
        $ge.GB_PAD_KEYBOARD_PLAYER = [string][Math]::Max(0, [Math]::Min(4, [int]$set.kbPlayer))
        $ge.GB_PAD_BACKGROUND = $(if ($set.padBg) { '1' } else { '0' })
    }
    # The keyboard's keys count with the controller layer off too.
    $keysIni = Join-Path $script:folder 'controls.ini'
    if (-not $ge.Contains('GB_PAD_CONFIG') -and (Test-Path -LiteralPath $keysIni)) { $ge.GB_PAD_CONFIG = $keysIni }
    # Sound while the game's window is not in front (port\src\gb_bgsound.c): on by default.
    if ($set.bgAudio -eq $false) { $ge.GB_BG_SOUND = '0' }
    # Game options (2026-11-16). A value the launcher sets cannot be changed in the
    # game's own menus, so the ones whose "on" is the game's default are sent only
    # when turned off, leaving the in-game controls working otherwise.
    if ($set.pauseBg -eq $false) { $ge.GB_PAUSE_BACKGROUND = '0' }         # the port's default is on
    if ($set.songRotate -eq $false) { $ge.GB_SONG_ROTATION = '0' }
    if ($set.rumbleOn -eq $false) { $ge.GB_RUMBLE = '0' }
    if ($set.demoDelay -match '^\d+$' -and $set.demoDelay -ne '90') { $ge.GB_DEMO_DELAY = [string]$set.demoDelay }   # 90 s is the game's own
    # Skip the intro film (GB_SKIP_INTRO, 2026-11-16): the port presses START in it.
    if ($set.skipIntro) { $ge.GB_SKIP_INTRO = '1' }
    # Game step rate (GB_GAME_HZ, 2026-11-16): 120 is the only rate measured in a
    # match, and it is experimental; 60 (unset) is the game's own.
    if ($set.gameHz -eq '120') { $ge.GB_GAME_HZ = '120' }
    # Crackle fix: the sound thread's 1 ms clock, a 40 ms lead, its own wait and
    # the guard that ends a late wait. On by default here; the game's own default stays off.
    if ($set.smoothAudio) { $ge.GB_AUDIO_TICK = '1'; $ge.GB_AUDIO_LEAD_MS = '40'; $ge.GB_AUDIO_WAIT = '1'; $ge.GB_AUDIO_GUARD = '1' }
    # Audio page (2026-11-16): each level is a percentage of the game's own,
    # read once at start and stacking with the game's Options sliders; 100 (or
    # unset) changes nothing, so a level is sent only when it was moved.
    $vols = [ordered]@{ volume = 'GB_AUDIO_VOLUME'; volMusic = 'GB_VOL_MUSIC'; volSfx = 'GB_VOL_SFX'; volCrowd = 'GB_VOL_CROWD'; volPlayers = 'GB_VOL_PLAYERS'; volCommentary = 'GB_VOL_COMMENTARY' }
    foreach ($k in $vols.Keys) {
        $v = $set.$k
        if ($null -ne $v -and "$v" -match '^\d+$' -and [int]$v -ge 0 -and [int]$v -lt 100) { $ge[$vols[$k]] = [string][int]$v }
    }
    if ($set.commentary -eq $false) { $ge.GB_COMMENTARY = '0'; $ge.Remove('GB_VOL_COMMENTARY') }
    if ($set.mono) { $ge.GB_AUDIO_MONO = '1' }
    # Cheats: with the master switch on,
    # the chosen ones as GB_CHEATS=a,b; nothing at all with it off.
    if ($set.cheats) {
        $chosen = @('street_kids', 'no_counters', 'gb_trails', 'all_quicks', 'unlimited_turbo', 'easy_2_pointers', 'hard_2_pointers',
                    'big_heads', 'wnba_ball', 'alternate_ball', 'no_hud', 'explosive_rims' | Where-Object { $set."cheat_$_" -eq $true })
        if ($chosen.Count) { $ge.GB_CHEATS = $chosen -join ',' }
    }
    # "Unlock everything": this one start only (the page never saves it).
    if ($set.unlockAll -eq $true) { $ge.GB_UNLOCK_ALL = '1' }
    # Frame counter (GB_SHOW_FPS): off unless chosen.
    if ($set.showFps) { $ge.GB_SHOW_FPS = '1' }
    # Texture dump for mod makers (GB_TEXTURE_DUMP, port\src\gb_textures.c): on at
    # start when chosen; Ctrl+F9 in the game toggles it either way. The game writes
    # to <install>\texture_dump, beside saves\.
    if ($set.texDump) { $ge.GB_TEXTURE_DUMP = '1' }
    # Mods: the game reads <install>\mods by itself (beside GB_SAVE_DIR); the
    # page's one switch turns every mod off for this start.
    if ($set.mods -eq $false) { $ge.GB_MODS = '0' }
    # The game always runs on Direct3D 11 from the launcher (the slow reference
    # renderer was removed from its settings, 2026-11-16; the game keeps it for
    # developers).
    if ($Mute) { $ge.GB_AUDIO_MUTE = '1' }
    # Saves and the game's log in the install folder (port\src\gb_save.c):
    # setup never deletes either, so Rebuild and Repair keep them.
    $ge.GB_SAVE_DIR = Join-Path $script:folder 'saves'
    $ge.GB_LOG_DIR = Join-Path $script:folder 'logs'
    $ge
}

function Start-Game([string]$json) {
    $set = $json | ConvertFrom-Json
    $exe = Join-Path $script:folder 'build\gamebreaker.exe'
    if (-not (Test-Path $exe)) { Send-Eng @{ ev = 'error'; step = 'play'; code = 'no_game'; text = "The game is not built in $($script:folder)." }; return }
    $full = (Resolve-Path $exe).Path
    if (Get-Process gamebreaker -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -eq $full } catch { $false } }) {
        Send-Eng @{ ev = 'error'; step = 'play'; code = 'running'; text = 'The game is already running.' }; return
    }
    $psi = New-Object Diagnostics.ProcessStartInfo($exe)
    $psi.WorkingDirectory = $script:folder
    $psi.UseShellExecute = $false
    $ge = Get-GameEnv $set
    foreach ($k in $ge.Keys) { $psi.EnvironmentVariables[$k] = $ge[$k] }
    if ($GameLog) {
        # Through cmd so the log goes to a file and the launcher can still wait on
        # one process (cmd ends when the game does).
        $logDir = Join-Path $script:folder 'logs'; New-Item -ItemType Directory -Force $logDir | Out-Null
        $psi.FileName = Join-Path $env:SystemRoot 'System32\cmd.exe'
        $psi.Arguments = '/c ""' + $exe + '" > "' + (Join-Path $logDir 'game_stdout.txt') + '" 2> "' + (Join-Path $logDir 'game_stderr.txt') + '""'
        $psi.CreateNoWindow = $true
    }
    # A program that will not start (damaged, or stopped by security software)
    # is reported as a Play problem; the page stays where it is.
    try { $p = [Diagnostics.Process]::Start($psi) }
    catch {
        Note ("game did not start: " + $_.Exception.Message)
        $why = $(if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message })
        Send-Eng @{ ev = 'error'; step = 'play'; code = 'start'; text = $why.Trim().TrimEnd('.') }
        return
    }
    Note ("game started, pid $($p.Id): " + (($ge.Keys | ForEach-Object { "$_=$($ge[$_])" }) -join ' '))
    # A new launcher waits for the game and opens when it ends; this one closes.
    Start-Process -FilePath 'conhost.exe' -ArgumentList (Get-RelaunchArgs $p.Id)
    $closeTimer.Start()
}
$closeTimer = New-Object Windows.Forms.Timer
$closeTimer.Interval = 1800        # let "Starting" show, then close completely
$closeTimer.add_Tick({ $closeTimer.Stop(); $form.Close() })

# Saves (port\src\gb_save.c): <install>\saves\UDATA\45410027\387E8D371AC2 holds
# the save's files; saves\backups\yyyy-mm-dd_hhmmss\ holds copies of them (the
# game makes one at start when the save changed, and keeps the newest five).
# Two kinds are kept apart from those five and never aged out:
# <date time>_before-unlock (before "Unlock everything") and
# <date time>_mods-changed (before a start whose set of mods differs).
$script:BackupName = '^\d{4}-\d\d-\d\d_\d{6}(_before-unlock|_mods-changed)?$'
function Get-SaveBackups {
    $bd = Join-Path $script:folder 'saves\backups'
    @(Get-ChildItem $bd -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match $script:BackupName } |
      Sort-Object Name -Descending | ForEach-Object Name)
}
function Test-GameRunning {
    $exe = Join-Path $script:folder 'build\gamebreaker.exe'
    [bool](Get-Process gamebreaker -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -eq $exe } catch { $false } })
}
function Restore-SaveBackup([string]$name) {
    $root = Join-Path $script:folder 'saves'
    $src = Join-Path $root "backups\$name"
    $dst = Join-Path $root 'UDATA\45410027\387E8D371AC2'
    if ($name -notmatch $script:BackupName -or -not (Test-Path $src)) { Send-Eng @{ ev = 'restored'; ok = $false; text = 'That backup is gone.' }; return }
    if (Test-GameRunning) { Send-Eng @{ ev = 'restored'; ok = $false; text = 'Close the game first.' }; return }
    try {
        # The current save first becomes a backup of its own (a save made in the
        # last session has none yet), named like the game's so it ages out the same way.
        $cur = @(Get-ChildItem $dst -File -ErrorAction SilentlyContinue)
        if ($cur) {
            $keep = Join-Path $root ("backups\" + (Get-Date -Format 'yyyy-MM-dd_HHmmss'))
            if (-not (Test-Path $keep)) { New-Item -ItemType Directory -Force $keep | Out-Null; $cur | Copy-Item -Destination $keep }
            $cur | Remove-Item -Force
        }
        New-Item -ItemType Directory -Force $dst | Out-Null
        Get-ChildItem $src -File | Copy-Item -Destination $dst -Force
        Note "save restored from $name (the previous save kept as a backup)"
        Send-Eng @{ ev = 'restored'; ok = $true }
    } catch {
        Note ("restore failed: " + $_.Exception.Message)
        Send-Eng @{ ev = 'restored'; ok = $false; text = 'The backup could not be restored.' }
    }
}

function Invoke-Live([string]$what) {
    Note "page asks: $what"
    $verb, $rest = $what -split ':', 2
    switch ($verb) {
        'check'  { Start-Engine 'check' @('-CheckOnly', '-Folder', $script:folder) }
        'folder' {
            $p = Select-Path 'install'
            if ($p) { $script:folder = Get-InstallChoice $p }
            # The page runs the checks again for a new folder (live:check), with Start held until they pass.
            Send-Eng @{ ev = 'folder'; path = $script:folder; changed = [bool]$p; installed = (Test-Installed $script:folder); tools = (Get-ToolList) }
        }
        'pick'   {
            $p = Select-Path $(if ($rest -eq 'folder') { 'disc' } else { 'file' })
            if (-not $p) { Send-Eng @{ ev = 'picked'; path = '' }; return }
            $script:disc = $p
            $size = 0; $isDir = (Get-Item -LiteralPath $p).PSIsContainer
            if (-not $isDir) { $size = (Get-Item -LiteralPath $p).Length }
            Send-Eng @{ ev = 'picked'; path = $p; name = (Split-Path $p -Leaf); size = $size; folder = $isDir }
            Start-Engine 'verify' @('-VerifyDisc', $p, '-Folder', $script:folder)
        }
        'setup'  {
            if (-not $TestFolder) { try { Set-Content -Path $script:folderFile -Value $script:folder -Encoding utf8 } catch { } }   # remembered for setup.cmd and the next start
            $a = @(); if ($script:disc) { $a += $script:disc }
            Start-Engine 'setup' ($a + @('-Folder', $script:folder))
        }
        'play'   { Start-Game $rest }
        'upcheck'   { Start-UpdateCheck }
        'upinstall' {
            if (Test-GameRunning) { Send-Eng @{ ev = 'error'; step = 'update'; code = 'running'; text = 'The game is running. Close it, then update.' }; return }
            Start-Engine 'upinstall' @('-Action', 'install', '-Folder', $script:folder) $script:updatePs
        }
        'upresume'  {
            # After the restart: build what the new files changed, or finish at once
            # when nothing is built yet (the first run builds everything anyway).
            if (Test-Installed $script:folder) { Start-Engine 'upbuild' @('-Folder', $script:folder) }
            else { Start-Engine 'upfinish' @('-Action', 'finish') $script:updatePs }
        }
        'checkfiles' { Start-Engine 'checkfiles' @('-Folder', $script:folder) (Join-Path $script:relRoot 'setup\check_files.ps1') }
        'movepick' {
            $p = Select-Path 'move'
            if (-not $p) { Send-Eng @{ ev = 'movepick'; to = '' }; return }
            $to = (Get-InstallChoice $p).TrimEnd('\')
            Send-Eng @{ ev = 'movepick'; from = $script:folder; to = $to }
        }
        'movego' {
            if (Test-GameRunning) { Send-Eng @{ ev = 'error'; step = 'move'; code = 'running'; text = 'The game is running. Close it, then move the folder.' }; return }
            $script:moveTo = $rest
            Start-Engine 'move' @('-From', $script:folder, '-To', $rest) (Join-Path $script:relRoot 'setup\move_install.ps1')
        }
        'uprollback' { Start-Engine 'uproll' @('-Action', 'rollback', '-Folder', $script:folder, '-Reason', 'The launcher closed while the files were being replaced.') $script:updatePs }
        'copylog' {
            # The latest setup log, then the end of the game's own log (logs\game.log,
            # port\src\gb_save.c) and this launcher's log: what a report needs.
            # log_report.ps1 leaves the player's profile and synced-folder names out.
            $report = Get-GbLogReport $script:folder $log
            if ($report) { [Windows.Forms.Clipboard]::SetText($report); Send-Eng @{ ev = 'copied'; ok = $true } } else { Send-Eng @{ ev = 'copied'; ok = $false } }
        }
        'opensaves' {
            $sv = Join-Path $script:folder 'saves'
            New-Item -ItemType Directory -Force $sv | Out-Null
            try { Start-Process explorer.exe -ArgumentList ('"' + $sv + '"'); Send-Eng @{ ev = 'saves'; ok = $true } }
            catch { Send-Eng @{ ev = 'saves'; ok = $false; text = 'The save folder could not be opened.' } }
        }
        'openshots' {
            # F12 in the game writes here (beside saves\, from GB_SAVE_DIR).
            $sh = Join-Path $script:folder 'screenshots'
            New-Item -ItemType Directory -Force $sh | Out-Null
            try { Start-Process explorer.exe -ArgumentList ('"' + $sh + '"'); Send-Eng @{ ev = 'saves'; ok = $true } }
            catch { Send-Eng @{ ev = 'saves'; ok = $false; text = 'The screenshots folder could not be opened.' } }
        }
        'opendump' {
            # The texture dump writes here (beside saves\, from GB_SAVE_DIR).
            $td = Join-Path $script:folder 'texture_dump'
            New-Item -ItemType Directory -Force $td | Out-Null
            try { Start-Process explorer.exe -ArgumentList ('"' + $td + '"'); Send-Eng @{ ev = 'saves'; ok = $true } }
            catch { Send-Eng @{ ev = 'saves'; ok = $false; text = 'The texture dump folder could not be opened.' } }
        }
        'backups' {
            $list = @(Get-SaveBackups)
            Send-Eng @{ ev = 'backups'; list = $list; text = 'No backups yet' }
        }
        'keysget' { Send-PadKeys }
        'keys' {
            try {
                $k = $rest | ConvertFrom-Json
                Set-PadKeys (Join-Path $script:folder 'controls.ini') ([int]$k.player) $k.keys
                Send-PadKeys
            } catch {
                Note ("keys not saved: " + $_.Exception.Message)
                Send-Eng @{ ev = 'keys'; ok = $false; text = 'The keys could not be saved.' }
            }
        }
        'restore' { Restore-SaveBackup $rest }
        'modsget' { Send-ModList }
        'modspick' { $p = Select-Path 'modzip'; if ($p) { Send-ModInspect $p } else { Send-EngObj ([ordered]@{ ev = 'modinspect'; cancelled = $true }) } }
        'modsforget' { $script:modZip = ''; $script:modToken = '' }
        'modsinstall' {
            $why = ''
            if (-not $script:modZip -or $rest -ne $script:modToken) { $why = 'Choose the mod again.' }
            elseif (Test-GameRunning) { $why = 'Close the game first.' }
            elseif ($script:eng -and -not $script:eng.HasExited) { $why = 'Setup is running.' }
            if ($why) { Send-EngObj ([ordered]@{ ev = 'modfail'; reason = $why }); return }
            $zip = $script:modZip; $script:modZip = ''; $script:modToken = ''
            Start-Engine 'modinstall' @('-Action', 'install', '-ModsDir', (Get-ModsDir), '-GameFiles', (Get-GameFilesDir), '-Zip', $zip) (Join-Path $script:relRoot 'setup\mods.ps1')
        }
        'modsremove' {
            if (Test-GameRunning) { Send-EngObj ([ordered]@{ ev = 'modremoved'; ok = $false; folder = $rest; reason = 'Close the game first.' }); return }
            $r = Remove-GbMod (Get-ModsDir) $rest
            Note ('mod removed: ' + $rest + ' (' + $(if ($r.ok) { if ($r.recycled) { 'Recycle Bin' } else { 'deleted' } } else { $r.reason }) + ')')
            Send-EngObj ([ordered]@{ ev = 'modremoved'; ok = [bool]$r.ok; folder = $rest; reason = $r.reason })
        }
        'modsorder' {
            try { Set-GbModOrder (Get-ModsDir) @(($rest | ConvertFrom-Json) | ForEach-Object { $_ }); Send-EngObj ([ordered]@{ ev = 'modorder'; ok = $true }) }
            catch { Note ('mod order not written: ' + $_.Exception.Message); Send-EngObj ([ordered]@{ ev = 'modorder'; ok = $false; reason = 'The order could not be saved.' }) }
        }
        'modsopen' {
            $md = Get-ModsDir; New-Item -ItemType Directory -Force $md | Out-Null
            $t = $md
            if ($rest -and -not (Test-GbModPath $rest) -and -not $rest.Contains('/')) { $c = Join-Path $md $rest; if (Test-Path -LiteralPath $c -PathType Container) { $t = $c } }
            try { Start-Process explorer.exe -ArgumentList ('"' + $t + '"') } catch { Send-EngObj ([ordered]@{ ev = 'modnote'; text = 'The folder could not be opened.' }) }
        }
        'stop'   { if ($script:eng -and -not $script:eng.HasExited) { Stop-Process -Id $script:eng.Id -Force -ErrorAction SilentlyContinue; Get-CimInstance Win32_Process -Filter "ParentProcessId=$($script:eng.Id)" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue } } }
        default  { Note "unknown request: $what" }
    }
}


$uri = New-Object Uri($Page)
if ($Start) { $uri = New-Object Uri($uri.AbsoluteUri + '#' + $Start) }
$view.Source = $uri


# Controller: polled here, sent to the page. Ignored while the window is not in front.
$padTimer = New-Object Windows.Forms.Timer
$padTimer.Interval = 16
$padTimer.add_Tick({
    if (-not $script:ready -or $script:inDialog) { return }
    $events = [GbHost.Pad]::Poll($clock.ElapsedMilliseconds)
    if ($events -and ($form.ContainsFocus -or $events.StartsWith('pad:'))) {
        $view.CoreWebView2.PostWebMessageAsString($events.TrimEnd("`n"))
    }
})
$padTimer.Start()


Note "window opening"
try { [Windows.Forms.Application]::Run($form) }
catch {
    Note ("stopped: " + $_.Exception.Message)
    Show-Stop $_.Exception.Message
}
if ($script:shotStream) { $script:shotStream.Dispose() }
if ($script:mutex) { try { $script:mutex.ReleaseMutex() } catch { } }
Note "closed"
if ($script:exitCode) { exit $script:exitCode }
