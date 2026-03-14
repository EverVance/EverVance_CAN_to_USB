Add-Type -AssemblyName System.Core

if (-not ("VbaCan.Debug.WinUsbSession" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace VbaCan.Debug
{
    public sealed class WinUsbSession
    {
        private static readonly Guid VbaCanInterfaceGuid = new Guid("620ABCCC-9C28-4D68-AA56-D7F27DE4B306");
        private const int PreferredVid = 0x1FC9;
        private const int PreferredPid = 0x0135;
        private readonly Queue<byte[]> _rx = new Queue<byte[]>();
        private readonly List<byte> _rxBuffer = new List<byte>(2048);

        private SafeFileHandle _deviceHandle;
        private IntPtr _winUsbHandle = IntPtr.Zero;
        private byte _bulkIn;
        private byte _bulkOut;

        public bool Open(string endpoint)
        {
            Close();

            int vid;
            int pid;
            ParseEndpoint(endpoint, out vid, out pid);
            string[] paths = FindDevicePaths(vid, pid);
            if (paths == null || paths.Length == 0)
            {
                return false;
            }

            for (int i = 0; i < paths.Length; i++)
            {
                string path = paths[i];
                _deviceHandle = Native.CreateFile(path, Native.GENERIC_READ | Native.GENERIC_WRITE,
                    Native.FILE_SHARE_READ | Native.FILE_SHARE_WRITE,
                    IntPtr.Zero, Native.OPEN_EXISTING,
                    Native.FILE_ATTRIBUTE_NORMAL | Native.FILE_FLAG_OVERLAPPED, IntPtr.Zero);
                if (_deviceHandle == null || _deviceHandle.IsInvalid)
                {
                    Close();
                    continue;
                }

                if (!Native.WinUsb_Initialize(_deviceHandle, out _winUsbHandle))
                {
                    Close();
                    continue;
                }

                if (!ResolveBulkPipes())
                {
                    Close();
                    continue;
                }

                uint timeout = 2;
                Native.WinUsb_SetPipePolicy(_winUsbHandle, _bulkIn, Native.PIPE_TRANSFER_TIMEOUT, 4, ref timeout);
                Native.WinUsb_SetPipePolicy(_winUsbHandle, _bulkOut, Native.PIPE_TRANSFER_TIMEOUT, 4, ref timeout);
                return true;
            }

            Close();
            return false;
        }

        public void Close()
        {
            IntPtr handle = _winUsbHandle;
            _winUsbHandle = IntPtr.Zero;
            if (handle != IntPtr.Zero)
            {
                Native.WinUsb_Free(handle);
            }

            SafeFileHandle dev = _deviceHandle;
            _deviceHandle = null;
            if (dev != null && !dev.IsInvalid)
            {
                dev.Close();
            }

            _rx.Clear();
            _rxBuffer.Clear();
        }

        public bool Send(byte[] packet)
        {
            if (_winUsbHandle == IntPtr.Zero || packet == null || packet.Length == 0)
            {
                return false;
            }

            uint written;
            if (!Native.WinUsb_WritePipe(_winUsbHandle, _bulkOut, packet, (uint)packet.Length, out written, IntPtr.Zero))
            {
                return false;
            }

            return written == packet.Length;
        }

        public byte[] TryReceive()
        {
            if (_rx.Count > 0)
            {
                return _rx.Dequeue();
            }

            if (_winUsbHandle == IntPtr.Zero)
            {
                return null;
            }

            byte[] tmp = new byte[512];
            uint read;
            if (Native.WinUsb_ReadPipe(_winUsbHandle, _bulkIn, tmp, (uint)tmp.Length, out read, IntPtr.Zero) && read > 0)
            {
                for (int i = 0; i < read; i++)
                {
                    _rxBuffer.Add(tmp[i]);
                }
                ParseRxFrames();
            }

            if (_rx.Count > 0)
            {
                return _rx.Dequeue();
            }

            return null;
        }

        private void ParseRxFrames()
        {
            while (_rxBuffer.Count >= 8)
            {
                if (_rxBuffer[0] != 0xA5)
                {
                    _rxBuffer.RemoveAt(0);
                    continue;
                }

                int dlc = _rxBuffer[2];
                if (dlc > 64)
                {
                    _rxBuffer.RemoveAt(0);
                    continue;
                }

                int length = 8 + dlc;
                if (_rxBuffer.Count < length)
                {
                    break;
                }

                byte[] frame = _rxBuffer.Take(length).ToArray();
                _rxBuffer.RemoveRange(0, length);
                _rx.Enqueue(frame);
            }
        }

        private bool ResolveBulkPipes()
        {
            Native.USB_INTERFACE_DESCRIPTOR ifDesc;
            if (!Native.WinUsb_QueryInterfaceSettings(_winUsbHandle, 0, out ifDesc))
            {
                return false;
            }

            byte inPipe = 0;
            byte outPipe = 0;
            for (byte i = 0; i < ifDesc.bNumEndpoints; i++)
            {
                Native.WINUSB_PIPE_INFORMATION pipe;
                if (!Native.WinUsb_QueryPipe(_winUsbHandle, 0, i, out pipe))
                {
                    continue;
                }

                if (pipe.PipeType != Native.USBD_PIPE_TYPE.UsbdPipeTypeBulk)
                {
                    continue;
                }

                if ((pipe.PipeId & 0x80) != 0)
                {
                    if (inPipe == 0)
                    {
                        inPipe = pipe.PipeId;
                    }
                }
                else if (outPipe == 0)
                {
                    outPipe = pipe.PipeId;
                }
            }

            if (inPipe == 0 || outPipe == 0)
            {
                return false;
            }

            _bulkIn = inPipe;
            _bulkOut = outPipe;
            return true;
        }

        private static string[] FindDevicePaths(int vid, int pid)
        {
            var result = new List<string>();
            if (vid < 0 || pid < 0)
            {
                vid = PreferredVid;
                pid = PreferredPid;
            }

            AppendUniquePath(result, FindDevicePathForGuid(VbaCanInterfaceGuid, vid, pid));
            if (result.Count > 0)
            {
                return result.ToArray();
            }

            foreach (Guid guid in EnumerateRegistryInterfaceGuids())
            {
                AppendUniquePath(result, FindDevicePathForGuid(guid, vid, pid));
            }

            AppendUniquePath(result, FindDevicePathForGuid(Native.GUID_DEVINTERFACE_USB_DEVICE, vid, pid));
            return result.ToArray();
        }

        private static void AppendUniquePath(List<string> paths, string path)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                return;
            }

            if (!paths.Any(existing => string.Equals(existing, path, StringComparison.OrdinalIgnoreCase)))
            {
                paths.Add(path);
            }
        }

        private static IEnumerable<Guid> EnumerateRegistryInterfaceGuids()
        {
            var seen = new HashSet<Guid>();
            using (RegistryKey usbRoot = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\USB"))
            {
                if (usbRoot == null)
                {
                    yield break;
                }

                foreach (string vidPidName in usbRoot.GetSubKeyNames())
                {
                    using (RegistryKey vidPidKey = usbRoot.OpenSubKey(vidPidName))
                    {
                        if (vidPidKey == null)
                        {
                            continue;
                        }

                        foreach (string instanceName in vidPidKey.GetSubKeyNames())
                        {
                            using (RegistryKey paramKey = vidPidKey.OpenSubKey(instanceName + @"\Device Parameters"))
                            {
                                if (paramKey == null)
                                {
                                    continue;
                                }

                                foreach (Guid guid in ReadInterfaceGuids(paramKey))
                                {
                                    if (seen.Add(guid))
                                    {
                                        yield return guid;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        private static IEnumerable<Guid> ReadInterfaceGuids(RegistryKey paramKey)
        {
            string[] multi = paramKey.GetValue("DeviceInterfaceGUIDs") as string[];
            if (multi != null)
            {
                for (int i = 0; i < multi.Length; i++)
                {
                    Guid guid;
                    if (Guid.TryParse(multi[i], out guid))
                    {
                        yield return guid;
                    }
                }
            }

            string single = paramKey.GetValue("DeviceInterfaceGUID") as string;
            Guid parsed;
            if (!string.IsNullOrWhiteSpace(single) && Guid.TryParse(single, out parsed))
            {
                yield return parsed;
            }
        }

        private static string FindDevicePathForGuid(Guid guid, int vid, int pid)
        {
            IntPtr info = Native.SetupDiGetClassDevs(ref guid, IntPtr.Zero, IntPtr.Zero,
                Native.DIGCF_PRESENT | Native.DIGCF_DEVICEINTERFACE);
            if (info == IntPtr.Zero || info.ToInt64() == -1)
            {
                return null;
            }

            try
            {
                uint index = 0;
                while (true)
                {
                    Native.SP_DEVICE_INTERFACE_DATA ifData = new Native.SP_DEVICE_INTERFACE_DATA();
                    ifData.cbSize = Marshal.SizeOf(ifData);
                    if (!Native.SetupDiEnumDeviceInterfaces(info, IntPtr.Zero, ref guid, index, ref ifData))
                    {
                        break;
                    }

                    uint required;
                    Native.SetupDiGetDeviceInterfaceDetail(info, ref ifData, IntPtr.Zero, 0, out required, IntPtr.Zero);
                    IntPtr detail = Marshal.AllocHGlobal((int)required);
                    try
                    {
                        Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                        if (!Native.SetupDiGetDeviceInterfaceDetail(info, ref ifData, detail, required, out required, IntPtr.Zero))
                        {
                            index++;
                            continue;
                        }

                        IntPtr pPath = IntPtr.Add(detail, 4);
                        string path = Marshal.PtrToStringAuto(pPath);
                        if (!string.IsNullOrWhiteSpace(path))
                        {
                            int pathVid;
                            int pathPid;
                            ExtractVidPid(path, out pathVid, out pathPid);
                            if ((vid < 0 || pathVid == vid) && (pid < 0 || pathPid == pid))
                            {
                                return path;
                            }
                        }
                    }
                    finally
                    {
                        Marshal.FreeHGlobal(detail);
                    }

                    index++;
                }
            }
            finally
            {
                Native.SetupDiDestroyDeviceInfoList(info);
            }

            return null;
        }

        private static void ParseEndpoint(string endpoint, out int vid, out int pid)
        {
            vid = -1;
            pid = -1;
            string text = (endpoint ?? string.Empty).Trim();
            int scheme = text.IndexOf("://", StringComparison.Ordinal);
            string body = scheme >= 0 ? text.Substring(scheme + 3) : text;
            int qmark = body.IndexOf('?');
            string head = qmark >= 0 ? body.Substring(0, qmark) : body;
            string query = qmark >= 0 ? body.Substring(qmark + 1) : string.Empty;

            if (!string.IsNullOrWhiteSpace(head) && !string.Equals(head, "auto", StringComparison.OrdinalIgnoreCase))
            {
                string[] parts = head.Split(':');
                if (parts.Length == 2)
                {
                    vid = ParseInt(parts[0], -1);
                    pid = ParseInt(parts[1], -1);
                }
            }

            foreach (string part in query.Split('&'))
            {
                int idx = part.IndexOf('=');
                if (idx <= 0)
                {
                    continue;
                }

                string key = part.Substring(0, idx);
                string value = part.Substring(idx + 1);
                if (string.Equals(key, "vid", StringComparison.OrdinalIgnoreCase))
                {
                    vid = ParseInt(value, vid);
                }
                else if (string.Equals(key, "pid", StringComparison.OrdinalIgnoreCase))
                {
                    pid = ParseInt(value, pid);
                }
            }
        }

        private static int ParseInt(string text, int dft)
        {
            string s = (text ?? string.Empty).Trim();
            int value;
            if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                if (int.TryParse(s.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out value))
                {
                    return value;
                }
            }
            if (int.TryParse(s, out value))
            {
                return value;
            }
            return dft;
        }

        private static void ExtractVidPid(string path, out int vid, out int pid)
        {
            vid = -1;
            pid = -1;
            if (string.IsNullOrWhiteSpace(path))
            {
                return;
            }

            string lower = path.ToLowerInvariant();
            int vidIndex = lower.IndexOf("vid_");
            if (vidIndex >= 0 && lower.Length >= vidIndex + 8)
            {
                int parsed;
                if (int.TryParse(lower.Substring(vidIndex + 4, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out parsed))
                {
                    vid = parsed;
                }
            }

            int pidIndex = lower.IndexOf("pid_");
            if (pidIndex >= 0 && lower.Length >= pidIndex + 8)
            {
                int parsed;
                if (int.TryParse(lower.Substring(pidIndex + 4, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out parsed))
                {
                    pid = parsed;
                }
            }
        }

        private static class Native
        {
            public const uint GENERIC_READ = 0x80000000;
            public const uint GENERIC_WRITE = 0x40000000;
            public const uint FILE_SHARE_READ = 0x00000001;
            public const uint FILE_SHARE_WRITE = 0x00000002;
            public const uint OPEN_EXISTING = 3;
            public const uint FILE_ATTRIBUTE_NORMAL = 0x00000080;
            public const uint FILE_FLAG_OVERLAPPED = 0x40000000;
            public const uint DIGCF_PRESENT = 0x00000002;
            public const uint DIGCF_DEVICEINTERFACE = 0x00000010;
            public const uint PIPE_TRANSFER_TIMEOUT = 0x03;

            public static readonly Guid GUID_DEVINTERFACE_USB_DEVICE =
                new Guid("A5DCBF10-6530-11D2-901F-00C04FB951ED");

            public enum USBD_PIPE_TYPE : int
            {
                UsbdPipeTypeControl,
                UsbdPipeTypeIsochronous,
                UsbdPipeTypeBulk,
                UsbdPipeTypeInterrupt
            }

            [StructLayout(LayoutKind.Sequential)]
            public struct WINUSB_PIPE_INFORMATION
            {
                public USBD_PIPE_TYPE PipeType;
                public byte PipeId;
                public ushort MaximumPacketSize;
                public byte Interval;
            }

            [StructLayout(LayoutKind.Sequential, Pack = 1)]
            public struct USB_INTERFACE_DESCRIPTOR
            {
                public byte bLength;
                public byte bDescriptorType;
                public byte bInterfaceNumber;
                public byte bAlternateSetting;
                public byte bNumEndpoints;
                public byte bInterfaceClass;
                public byte bInterfaceSubClass;
                public byte bInterfaceProtocol;
                public byte iInterface;
            }

            [StructLayout(LayoutKind.Sequential)]
            public struct SP_DEVICE_INTERFACE_DATA
            {
                public int cbSize;
                public Guid InterfaceClassGuid;
                public int Flags;
                public IntPtr Reserved;
            }

            [DllImport("setupapi.dll", SetLastError = true)]
            public static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, IntPtr enumerator, IntPtr hwndParent, uint flags);

            [DllImport("setupapi.dll", SetLastError = true)]
            public static extern bool SetupDiEnumDeviceInterfaces(IntPtr deviceInfoSet, IntPtr deviceInfoData, ref Guid interfaceClassGuid, uint memberIndex, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData);

            [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Auto)]
            public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr deviceInfoSet, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData,
                IntPtr deviceInterfaceDetailData, uint deviceInterfaceDetailDataSize, out uint requiredSize, IntPtr deviceInfoData);

            [DllImport("setupapi.dll", SetLastError = true)]
            public static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

            [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
            public static extern SafeFileHandle CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
                IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_Initialize(SafeFileHandle deviceHandle, out IntPtr interfaceHandle);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_Free(IntPtr interfaceHandle);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_QueryInterfaceSettings(IntPtr interfaceHandle, byte alternateSettingNumber, out USB_INTERFACE_DESCRIPTOR usbAltInterfaceDescriptor);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_QueryPipe(IntPtr interfaceHandle, byte alternateInterfaceNumber, byte pipeIndex, out WINUSB_PIPE_INFORMATION pipeInformation);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_SetPipePolicy(IntPtr interfaceHandle, byte pipeId, uint policyType, uint valueLength, ref uint value);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_ReadPipe(IntPtr interfaceHandle, byte pipeId, byte[] buffer, uint bufferLength, out uint lengthTransferred, IntPtr overlapped);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_WritePipe(IntPtr interfaceHandle, byte pipeId, byte[] buffer, uint bufferLength, out uint lengthTransferred, IntPtr overlapped);
        }
    }
}
"@
}

function New-WinUsbTransport {
    param(
        [string]$Endpoint = 'winusb://auto'
    )

    $session = New-Object VbaCan.Debug.WinUsbSession

    $obj = [PSCustomObject]@{
        Name = 'winusb'
        Open = {
            param()
            return $session.Open($Endpoint)
        }
        Close = {
            param()
            $session.Close()
            return $true
        }
        Send = {
            param([byte[]]$data)
            return $session.Send($data)
        }
        TryReceive = {
            param()
            $packet = $session.TryReceive()
            if ($null -eq $packet) {
                return $null
            }
            return ,$packet
        }
    }

    return $obj
}
