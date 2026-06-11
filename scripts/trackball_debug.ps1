<#
.SYNOPSIS
    Charybdis Trackball Telemetry & Stutter Console for Windows.
.DESCRIPTION
    Monitors raw mouse input events globally in user space (no Admin/root required)
    and reports micro-stutters/lags during active movement.
.PARAMETER Quiet
    Only print warnings when a stutter is detected.
.PARAMETER VerboseInput
    Print every motion packet (caution: high console output rate).
.PARAMETER StutterThresholdMs
    Threshold in milliseconds to flag a delay as a stutter (default: 50).
.EXAMPLE
    .\trackball_debug.ps1 -Quiet
#>
[CmdletBinding()]
param(
    [Alias('q', 'e')]
    [switch]$Quiet,

    [Alias('v')]
    [switch]$VerboseInput,

    [int]$StutterThresholdMs = 50
)

# Set console encoding to UTF8 for clean icons
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# Load Required assemblies
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# Define friendly name mapper
function Get-FriendlyName {
    param([string]$deviceName)
    # Windows Raw Input device names look like:
    # \\?\HID#VID_1D50&PID_615E&MI_01#7&1bc842bb&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}
    if ($deviceName -match "VID_([0-9A-F]+)&PID_([0-9A-F]+)") {
        $vid = $Matches[1]
        $pid = $Matches[2]
        
        # ZMK/nice!nano default Vendor/Product IDs
        if ($vid -eq "1D50" -and $pid -eq "615E") {
            return "Charybdis Trackball (ZMK/nice!nano)"
        }
        return "HID Mouse [VID:$vid PID:$pid]"
    }
    return "Standard Mouse / Touchpad"
}

# Compile C# Raw Input receiver wrapper
$cSharpCode = @"
using System;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using System.Collections.Generic;

public class DeviceInfo
{
    public IntPtr Handle { get; set; }
    public string Name { get; set; }
    public int Hz { get; set; }
}

public class RawInputReceiver : Form
{
    private const int WM_INPUT = 0x00FF;
    private const int RID_INPUT = 0x10000003;
    private const int RIM_TYPEMOUSE = 0;

    [DllImport("user32.dll")]
    private static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] pRawInputDevices, uint uiNumDevices, uint cbSize);

    [DllImport("user32.dll")]
    private static extern uint GetRawInputData(IntPtr hRawInput, uint uiCommand, IntPtr pData, ref uint pcbSize, uint cbSizeHeader);

    [DllImport("user32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    private static extern uint GetRawInputDeviceInfo(IntPtr hDevice, uint uiCommand, IntPtr pData, ref uint pcbSize);

    [StructLayout(LayoutKind.Sequential)]
    public struct RAWINPUTDEVICE
    {
        public ushort usUsagePage;
        public ushort usUsage;
        public uint dwFlags;
        public IntPtr hwndTarget;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RAWINPUTHEADER
    {
        public uint dwType;
        public uint dwSize;
        public IntPtr hDevice;
        public IntPtr wParam;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct RAWMOUSE
    {
        [FieldOffset(0)] public ushort usFlags;
        [FieldOffset(4)] public uint ulButtons;
        [FieldOffset(4)] public ushort usButtonFlags;
        [FieldOffset(6)] public ushort usButtonData;
        [FieldOffset(8)] public uint ulRawButtons;
        [FieldOffset(12)] public int lLastX;
        [FieldOffset(16)] public int lLastY;
        [FieldOffset(20)] public uint ulExtraInformation;
    }

    public class DeviceState
    {
        public IntPtr Handle;
        public string Name;
        public Queue<DateTime> PacketTimes = new Queue<DateTime>();
        public DateTime LastPacketTime = DateTime.MinValue;
        public DateTime PrevPacketTime = DateTime.MinValue;
        public DateTime PrevPrevPacketTime = DateTime.MinValue;
        public int PacketCount = 0;
        public int Hz = 0;
    }

    public delegate void StutterEventHandler(IntPtr hDevice, string deviceName, double delayMs, int lastX, int lastY);
    public event StutterEventHandler OnStutterDetected;

    public delegate void DataEventHandler(IntPtr hDevice, string deviceName, int lastX, int lastY, int hz);
    public event DataEventHandler OnDataReceived;

    public delegate void StatsEventHandler();
    public event StatsEventHandler OnStatsUpdated;

    private Dictionary<IntPtr, DeviceState> _devices = new Dictionary<IntPtr, DeviceState>();
    private Dictionary<IntPtr, string> _deviceNames = new Dictionary<IntPtr, string>();
    private double _stutterThreshold = 50.0;
    private Timer _statsTimer;

    public double StutterThreshold
    {
        get { return _stutterThreshold; }
        set { _stutterThreshold = value; }
    }

    public RawInputReceiver()
    {
        this.Width = 0;
        this.Height = 0;
        this.ShowInTaskbar = false;
        this.WindowState = FormWindowState.Minimized;
        this.FormBorderStyle = FormBorderStyle.None;
        this.Opacity = 0;

        _statsTimer = new Timer();
        _statsTimer.Interval = 1000;
        _statsTimer.Tick += (s, ev) => {
            DateTime now = DateTime.Now;
            lock (_devices)
            {
                foreach (var kvp in _devices)
                {
                    var state = kvp.Value;
                    while (state.PacketTimes.Count > 0 && (now - state.PacketTimes.Peek()).TotalMilliseconds > 1000)
                    {
                        state.PacketTimes.Dequeue();
                    }
                    state.Hz = state.PacketTimes.Count;
                }
            }
            if (OnStatsUpdated != null)
            {
                OnStatsUpdated();
            }
        };
        _statsTimer.Start();
    }

    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);

        RAWINPUTDEVICE[] rid = new RAWINPUTDEVICE[1];
        rid[0].usUsagePage = 1; 
        rid[0].usUsage = 2;     
        rid[0].dwFlags = 0x00000100; // RIDEV_INPUTSINK (capture background events)
        rid[0].hwndTarget = this.Handle;

        RegisterRawInputDevices(rid, 1, (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICE)));
    }

    private string GetDeviceName(IntPtr hDevice)
    {
        if (_deviceNames.TryGetValue(hDevice, out string name))
        {
            return name;
        }

        uint size = 0;
        GetRawInputDeviceInfo(hDevice, 0x20000007, IntPtr.Zero, ref size);
        if (size > 0)
        {
            IntPtr buffer = Marshal.AllocHGlobal((int)size * 2);
            try
            {
                if (GetRawInputDeviceInfo(hDevice, 0x20000007, buffer, ref size) != 0xFFFFFFFF)
                {
                    name = Marshal.PtrToStringUni(buffer);
                    _deviceNames[hDevice] = name;
                    return name;
                }
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }
        return "Unknown Device";
    }

    protected override void WndProc(ref Message m)
    {
        if (m.Msg == WM_INPUT)
        {
            uint dwSize = 0;
            GetRawInputData(m.LParam, RID_INPUT, IntPtr.Zero, ref dwSize, (uint)Marshal.SizeOf(typeof(RAWINPUTHEADER)));

            if (dwSize > 0)
            {
                IntPtr buffer = Marshal.AllocHGlobal((int)dwSize);
                try
                {
                    if (GetRawInputData(m.LParam, RID_INPUT, buffer, ref dwSize, (uint)Marshal.SizeOf(typeof(RAWINPUTHEADER))) == dwSize)
                    {
                        int headerSize = (IntPtr.Size == 8) ? 24 : 16;
                        RAWINPUTHEADER header = (RAWINPUTHEADER)Marshal.PtrToStructure(buffer, typeof(RAWINPUTHEADER));
                        
                        if (header.dwType == RIM_TYPEMOUSE)
                        {
                            IntPtr mousePtr = (IntPtr)((long)buffer + headerSize);
                            RAWMOUSE mouse = (RAWMOUSE)Marshal.PtrToStructure(mousePtr, typeof(RAWMOUSE));
                            
                            string devName = GetDeviceName(header.hDevice);
                            ProcessMouseInput(header.hDevice, devName, mouse.lLastX, mouse.lLastY, mouse.usButtonFlags);
                        }
                    }
                }
                finally
                {
                    Marshal.FreeHGlobal(buffer);
                }
            }
        }
        base.WndProc(ref m);
    }

    private void ProcessMouseInput(IntPtr hDevice, string deviceName, int lastX, int lastY, ushort buttonFlags)
    {
        DateTime now = DateTime.Now;
        DeviceState state;
        
        lock (_devices)
        {
            if (!_devices.TryGetValue(hDevice, out state))
            {
                state = new DeviceState { Handle = hDevice, Name = deviceName };
                _devices[hDevice] = state;
            }
        }

        state.PacketCount++;
        state.PacketTimes.Enqueue(now);

        if (state.LastPacketTime != DateTime.MinValue)
        {
            double dt_current = (now - state.LastPacketTime).TotalMilliseconds;
            
            if (state.PrevPacketTime != DateTime.MinValue)
            {
                double dt_prev = (state.LastPacketTime - state.PrevPacketTime).TotalMilliseconds;
                
                // Stutter detection heuristic:
                // Current report gap > threshold (50ms) AND previous gap was small (< 25ms, active movement) AND there was movement.
                if (dt_current > _stutterThreshold && dt_prev < 25.0 && (lastX != 0 || lastY != 0))
                {
                    if (OnStutterDetected != null)
                    {
                        OnStutterDetected(hDevice, deviceName, dt_current, lastX, lastY);
                    }
                }
            }
        }

        state.PrevPrevPacketTime = state.PrevPacketTime;
        state.PrevPacketTime = state.LastPacketTime;
        state.LastPacketTime = now;

        if (OnDataReceived != null)
        {
            OnDataReceived(hDevice, deviceName, lastX, lastY, state.Hz);
        }
    }

    public List<DeviceInfo> GetDevices()
    {
        var list = new List<DeviceInfo>();
        lock (_devices)
        {
            foreach (var kvp in _devices)
            {
                list.Add(new DeviceInfo { Handle = kvp.Value.Handle, Name = kvp.Value.Name, Hz = kvp.Value.Hz });
            }
        }
        return list;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _statsTimer.Stop();
            _statsTimer.Dispose();
        }
        base.Dispose(disposing);
    }
}
"@

# Compile the class
Add-Type -TypeDefinition $cSharpCode -ReferencedAssemblies "System.Windows.Forms", "System.Drawing"

# Instantiate receiver
$receiver = New-Object RawInputReceiver
$receiver.StutterThreshold = $StutterThresholdMs

# Event Handlers
$receiver.add_OnStutterDetected({
    param($hDevice, $deviceName, $delayMs, $lastX, $lastY)
    $friendly = Get-FriendlyName $deviceName
    $timestamp = Get-Date -Format "HH:mm:ss"
    Write-Host ("[{0}] ⚠️  [STUTTER] Lag detected on {1}! Delay of {2:F1} ms during active movement (dx: {3}, dy: {4})" -f $timestamp, $friendly, $delayMs, $lastX, $lastY) -ForegroundColor Yellow
})

if ($VerboseInput) {
    $receiver.add_OnDataReceived({
        param($hDevice, $deviceName, $lastX, $lastY, $hz)
        $friendly = Get-FriendlyName $deviceName
        $timestamp = Get-Date -Format "HH:mm:ss"
        Write-Host ("[{0}] ⚡ [DATA] {1}: dx={2}, dy={3} | Rate: {4} Hz" -f $timestamp, $friendly, $lastX, $lastY, $hz) -ForegroundColor Green
    })
}

$receiver.add_OnStatsUpdated({
    # Update live report rate using Write-Progress at the top of the console
    $devices = $receiver.GetDevices()
    $i = 1
    foreach ($dev in $devices) {
        $friendly = Get-FriendlyName $dev.Name
        Write-Progress -Id $i -Activity "Charybdis Trackball Telemetry" -Status "Device: $friendly | Live Rate: $($dev.Hz) Hz" -PercentComplete -1
        $i++
    }
})

# Display landing page
Clear-Host
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "  🔮 CHARYBDIS MINI WIRELESS — WINDOWS TELEMETRY CONSOLE" -ForegroundColor Cyan
Write-Host "  Heuristic Stutter Filter Active (Threshold: $StutterThresholdMs ms)" -ForegroundColor Cyan
if ($Quiet) {
    Write-Host "  Mode:            ⚠️  Quiet (Errors & Lags Only)" -ForegroundColor Yellow
} else {
    Write-Host "  Mode:            📊 Live Status Update Active" -ForegroundColor Green
}
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "  Press Ctrl + C to exit this console at any time."
Write-Host "  Move your trackball to see active report rates at the top..."
Write-Host ""

# Console interrupt handler for graceful cleanup
$cancelHandler = [ConsoleCancelEventHandler]{
    param($sender, $eventArgs)
    $eventArgs.Cancel = $true
    [System.Windows.Forms.Application]::Exit()
}
[Console]::add_CancelKeyPress($cancelHandler)

try {
    # Run the Windows Form Message Loop
    [System.Windows.Forms.Application]::Run($receiver)
}
finally {
    [Console]::remove_CancelKeyPress($cancelHandler)
    
    # Hide any remaining progress bars
    for ($i = 1; $i -le 10; $i++) {
        Write-Progress -Id $i -Activity "Cleanup" -Completed
    }
    
    $receiver.Dispose()
    Write-Host "`n[+] Telemetry console closed and resources disposed." -ForegroundColor Green
}
