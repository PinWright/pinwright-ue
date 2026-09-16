# Copyright (c) 2026 Alexander Penkin. MIT License.
<#
.SYNOPSIS
    Shared Windows Job Object interop for the capped launchers. Dot-source it; it defines a type
    and one helper function and runs nothing on its own.

.DESCRIPTION
    Run-SuiteCapped.ps1 (the editor suite) and Run-Capped.ps1 (any command) need the same thing:
    create a job, set its limits, create the child SUSPENDED, assign, resume. One copy of the
    P/Invoke here means the two launchers cannot drift on the ordering that makes the cap real.

    Safe to dot-source repeatedly -- the Add-Type is guarded, so a second dot-source in the same
    session reuses the already-compiled type rather than failing on a duplicate definition.
#>

# ---------------------------------------------------------------------------------------------
# Job Object interop. Add-Type once per session; a re-run in the same session reuses the type.
# ---------------------------------------------------------------------------------------------
if (-not ('PinWrightCappedRun' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public class PinWrightCappedRun
{
    public const uint JOB_OBJECT_LIMIT_PROCESS_MEMORY    = 0x00000100;
    public const uint JOB_OBJECT_LIMIT_PRIORITY_CLASS    = 0x00000020;
    public const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;

    // Win32 priority classes. Same values CreateProcess takes in dwCreationFlags, but here they go
    // into BasicLimitInformation.PriorityClass, which holds one class -- not a flag mask.
    public const uint NORMAL_PRIORITY_CLASS       = 0x00000020;
    public const uint IDLE_PRIORITY_CLASS         = 0x00000040;
    public const uint BELOW_NORMAL_PRIORITY_CLASS = 0x00004000;

    const uint CREATE_SUSPENDED = 0x00000004;
    const uint CREATE_NO_WINDOW = 0x08000000;
    const int  JobObjectExtendedLimitInformation = 9;
    const int  JobObjectLimitViolationInformation = 13;
    const uint WAIT_OBJECT_0 = 0;
    const uint WAIT_TIMEOUT  = 258;

    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct IO_COUNTERS
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_LIMIT_VIOLATION_INFORMATION
    {
        public uint LimitFlags;
        public uint ViolationLimitFlags;
        public ulong IoReadBytes;
        public ulong IoReadBytesLimit;
        public ulong IoWriteBytes;
        public ulong IoWriteBytesLimit;
        public long PerJobUserTime;
        public long PerJobUserTimeLimit;
        public ulong JobMemory;
        public ulong JobMemoryLimit;
        public uint RateControlTolerance;
        public uint RateControlToleranceLimit;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct STARTUPINFO
    {
        public int cb;
        public IntPtr lpReserved;
        public IntPtr lpDesktop;
        public IntPtr lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute;
        public int dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_INFORMATION
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public uint dwProcessId;
        public uint dwThreadId;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcess(string lpApplicationName, StringBuilder lpCommandLine,
        IntPtr lpProcessAttributes, IntPtr lpThreadAttributes, bool bInheritHandles,
        uint dwCreationFlags, IntPtr lpEnvironment, string lpCurrentDirectory,
        ref STARTUPINFO lpStartupInfo, out PROCESS_INFORMATION lpProcessInformation);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateJobObject(IntPtr lpJobAttributes, string lpName);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetInformationJobObject(IntPtr hJob, int infoClass, IntPtr lpInfo, uint cbInfo);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool QueryInformationJobObject(IntPtr hJob, int infoClass, IntPtr lpInfo,
        uint cbInfo, IntPtr lpReturnLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint ResumeThread(IntPtr hThread);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeProcess(IntPtr hProcess, out uint lpExitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool TerminateProcess(IntPtr hProcess, uint uExitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr hObject);

    IntPtr _job = IntPtr.Zero;
    IntPtr _process = IntPtr.Zero;
    IntPtr _thread = IntPtr.Zero;

    public uint ProcessId;
    public ulong CapBytes;
    // What the job actually carries, so a caller reports the applied class rather than the asked-for one.
    public uint PriorityClass;
    // Honest reporting: the violation query is only populated when the job carries NOTIFICATION
    // limits, so a hard-limit job can legitimately answer nothing. Say so rather than print 0x0
    // as if it were a measurement.
    public bool ViolationQueryed;
    public uint ViolationLimitFlags;

    public static PinWrightCappedRun Start(string exe, string commandLine, ulong memoryLimitBytes, uint priorityClass)
    {
        PinWrightCappedRun run = new PinWrightCappedRun();
        run.CapBytes = memoryLimitBytes;
        run.PriorityClass = priorityClass;

        run._job = CreateJobObject(IntPtr.Zero, null);
        if (run._job == IntPtr.Zero) { throw new Exception("CreateJobObject failed: " + Marshal.GetLastWin32Error()); }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        limits.ProcessMemoryLimit = new UIntPtr(memoryLimitBytes);
        // WHY THE JOB LIMIT AND NOT SetPriorityClass ON THE CHILD. JOB_OBJECT_LIMIT_PRIORITY_CLASS
        // applies to every process in the job, including children the command spawns later
        // (ShaderCompileWorker for the editor; cl.exe/link.exe/dotnet UnrealBuildTool for a build),
        // and a process inside the job cannot raise its own priority above the job's -- UBT hands
        // NORMAL_PRIORITY_CLASS to CreateProcess for every action it spawns (EpicGames.Core
        // ManagedProcess.cs:659-679) and the job limit overrides that.
        // Nothing on that path escapes: neither ManagedProcess nor UnrealBuildAccelerator passes
        // CREATE_BREAKAWAY_FROM_JOB. ManagedProcessGroup sets only JOB_OBJECT_LIMIT_BREAKAWAY_OK on
        // its OWN nested job (ManagedProcess.cs:144-155), which permits breakaway but never requests
        // it, so build children stay inside the outer job and inherit its class.
        if (priorityClass != 0)
        {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PRIORITY_CLASS;
            limits.BasicLimitInformation.PriorityClass = priorityClass;
        }
        int size = Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
        IntPtr buffer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(limits, buffer, false);
            if (!SetInformationJobObject(run._job, JobObjectExtendedLimitInformation, buffer, (uint)size))
            {
                throw new Exception("SetInformationJobObject failed: " + Marshal.GetLastWin32Error());
            }
        }
        finally { Marshal.FreeHGlobal(buffer); }

        STARTUPINFO si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        PROCESS_INFORMATION pi;
        // Suspended, so the job owns the process before it can allocate its first byte. Assigning
        // a running process would leave a window in which the cap does not apply -- and UE reads
        // the limit once, at startup.
        // Extra capacity because CreateProcessW is documented to be able to modify lpCommandLine
        // in place.
        StringBuilder mutableCommandLine = new StringBuilder(commandLine, commandLine.Length + 32);
        if (!CreateProcess(exe, mutableCommandLine, IntPtr.Zero, IntPtr.Zero, false,
                CREATE_SUSPENDED | CREATE_NO_WINDOW, IntPtr.Zero, null, ref si, out pi))
        {
            int err = Marshal.GetLastWin32Error();
            CloseHandle(run._job);
            run._job = IntPtr.Zero;
            throw new Exception("CreateProcess failed: " + err);
        }

        run._process = pi.hProcess;
        run._thread = pi.hThread;
        run.ProcessId = pi.dwProcessId;

        if (!AssignProcessToJobObject(run._job, run._process))
        {
            int err = Marshal.GetLastWin32Error();
            TerminateProcess(run._process, 1);
            run.Close();
            throw new Exception("AssignProcessToJobObject failed: " + err);
        }

        if (ResumeThread(run._thread) == 0xFFFFFFFF)
        {
            int err = Marshal.GetLastWin32Error();
            TerminateProcess(run._process, 1);
            run.Close();
            throw new Exception("ResumeThread failed: " + err);
        }

        return run;
    }

    /// <summary>Blocks up to timeoutMs. True when the process exited.</summary>
    public bool Wait(int timeoutMs)
    {
        return WaitForSingleObject(_process, (uint)timeoutMs) == WAIT_OBJECT_0;
    }

    public int ExitCode()
    {
        uint code;
        if (!GetExitCodeProcess(_process, out code)) { return -1; }
        return unchecked((int)code);
    }

    public void Kill()
    {
        TerminateProcess(_process, 1);
        WaitForSingleObject(_process, 15000);
    }

    /// <summary>Peak commit of the largest process ever in the job, in bytes. 0 when unreadable.</summary>
    public ulong PeakProcessMemory()
    {
        int size = Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
        IntPtr buffer = Marshal.AllocHGlobal(size);
        try
        {
            if (!QueryInformationJobObject(_job, JobObjectExtendedLimitInformation, buffer, (uint)size, IntPtr.Zero))
            {
                return 0;
            }
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info =
                (JOBOBJECT_EXTENDED_LIMIT_INFORMATION)Marshal.PtrToStructure(buffer, typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
            return info.PeakProcessMemoryUsed.ToUInt64();
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    public void ReadViolations()
    {
        int size = Marshal.SizeOf(typeof(JOBOBJECT_LIMIT_VIOLATION_INFORMATION));
        IntPtr buffer = Marshal.AllocHGlobal(size);
        try
        {
            ViolationQueryed = QueryInformationJobObject(_job, JobObjectLimitViolationInformation, buffer, (uint)size, IntPtr.Zero);
            if (ViolationQueryed)
            {
                JOBOBJECT_LIMIT_VIOLATION_INFORMATION info =
                    (JOBOBJECT_LIMIT_VIOLATION_INFORMATION)Marshal.PtrToStructure(buffer, typeof(JOBOBJECT_LIMIT_VIOLATION_INFORMATION));
                ViolationLimitFlags = info.ViolationLimitFlags;
            }
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    /// <summary>Closing the job kills anything still in it -- stray shader workers included.</summary>
    public void Close()
    {
        if (_thread != IntPtr.Zero) { CloseHandle(_thread); _thread = IntPtr.Zero; }
        if (_process != IntPtr.Zero) { CloseHandle(_process); _process = IntPtr.Zero; }
        if (_job != IntPtr.Zero) { CloseHandle(_job); _job = IntPtr.Zero; }
    }
}
'@
}

function Get-PinWrightPriorityFlag {
    <#
    .SYNOPSIS
        Maps Normal|BelowNormal|Idle onto the Win32 priority class the job limit stores.
    #>
    [CmdletBinding()]
    [OutputType([uint32])]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('Normal', 'BelowNormal', 'Idle')]
        [string] $Name
    )

    # BelowNormal is the default everywhere, deliberately not Idle: an IDLE_PRIORITY_CLASS job is
    # descheduled whenever anything else on the box wants CPU, so a background suite or build can be
    # starved for its whole run and its wall time stops measuring anything. BelowNormal yields to
    # interactive work without starving.
    switch ($Name) {
        'Normal'      { return [PinWrightCappedRun]::NORMAL_PRIORITY_CLASS }
        'BelowNormal' { return [PinWrightCappedRun]::BELOW_NORMAL_PRIORITY_CLASS }
        'Idle'        { return [PinWrightCappedRun]::IDLE_PRIORITY_CLASS }
    }
}
