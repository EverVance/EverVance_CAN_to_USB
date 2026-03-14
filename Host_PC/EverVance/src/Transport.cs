using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
using System.Threading;

namespace EverVance
{
    public interface ITransport
    {
        bool Open(string endpoint);
        void Close();
        bool Send(byte[] packet);
        bool TryReceive(out byte[] packet);
        string Name { get; }
    }

    public static class TransportFactory
    {
        public static ITransport Create(string endpoint)
        {
            var ep = (endpoint ?? "").Trim();
            if (ep.StartsWith("winusb://", StringComparison.OrdinalIgnoreCase) ||
                ep.StartsWith("usb://", StringComparison.OrdinalIgnoreCase))
            {
                return new WinUsbTransport();
            }
            return new MockTransport();
        }
    }

    public sealed class MockTransport : ITransport
    {
        private readonly ConcurrentQueue<byte[]> _rx = new ConcurrentQueue<byte[]>();
        private Timer _simTimer;
        private long _tick;
        private volatile bool _opened;
        private int _simPeriodMs = 100;

        public string Name { get { return "Mock"; } }

        public bool Open(string endpoint)
        {
            _simPeriodMs = ParseMockPeriod(endpoint, 100);
            _opened = true;
            StartSimulator();
            return true;
        }

        public void Close()
        {
            _opened = false;
            StopSimulator();
        }

        public bool Send(byte[] packet)
        {
            if (packet == null) return false;
            _rx.Enqueue(packet);
            return true;
        }

        public bool TryReceive(out byte[] packet)
        {
            return _rx.TryDequeue(out packet);
        }

        private void StartSimulator()
        {
            if (_simTimer != null) return;
            _simTimer = new Timer(delegate { ProduceDemoFrames(); }, null, _simPeriodMs, _simPeriodMs);
        }

        private void StopSimulator()
        {
            var timer = _simTimer;
            _simTimer = null;
            if (timer != null) timer.Dispose();
        }

        private void ProduceDemoFrames()
        {
            if (!_opened) return;

            var t = Interlocked.Increment(ref _tick);
            var msgSelector = (int)(t % 4);

            if (msgSelector == 0)
            {
                var ch0 = (byte)(t % 4);
                ushort speedRaw = (ushort)((t * 3) % 12000);
                ushort rpmRaw = (ushort)(2000 + ((t * 17) % 12000));
                byte accRaw = (byte)((t * 2) % 250);
                byte brakeRaw = (byte)(((t / 3) % 80));
                byte[] vcu = new byte[8];
                vcu[0] = (byte)(speedRaw & 0xFF);
                vcu[1] = (byte)((speedRaw >> 8) & 0xFF);
                vcu[2] = (byte)(rpmRaw & 0xFF);
                vcu[3] = (byte)((rpmRaw >> 8) & 0xFF);
                vcu[4] = accRaw;
                vcu[5] = brakeRaw;
                vcu[6] = (byte)((t / 50) & 0x0F);
                vcu[6] |= (byte)(((t / 100) & 0x07) << 4);
                vcu[6] |= (byte)(((t / 200) & 0x01) << 7);
                _rx.Enqueue(BuildPacket(ch0, 0x100, 0x00, vcu));
            }
            else if (msgSelector == 1)
            {
                byte ch1 = (byte)((t + 1) % 4);
                ushort ws0 = (ushort)((t * 3 + 10) % 12000);
                ushort ws1 = (ushort)((t * 3 + 25) % 12000);
                ushort ws2 = (ushort)((t * 3 + 8) % 12000);
                ushort ws3 = (ushort)((t * 3 + 18) % 12000);
                byte[] abs = new byte[8];
                abs[0] = (byte)(ws0 & 0xFF); abs[1] = (byte)(ws0 >> 8);
                abs[2] = (byte)(ws1 & 0xFF); abs[3] = (byte)(ws1 >> 8);
                abs[4] = (byte)(ws2 & 0xFF); abs[5] = (byte)(ws2 >> 8);
                abs[6] = (byte)(ws3 & 0xFF); abs[7] = (byte)(ws3 >> 8);
                _rx.Enqueue(BuildPacket(ch1, 0x110, 0x00, abs));
            }
            else if (msgSelector == 2)
            {
                byte ch2 = (byte)((t + 2) % 4);
                short steerAngleRaw = (short)(((t % 600) - 300) * 5);
                short steerTorqueRaw = (short)(((t % 200) - 100) * 3);
                byte[] eps = new byte[8];
                eps[0] = (byte)(steerAngleRaw & 0xFF); eps[1] = (byte)((steerAngleRaw >> 8) & 0xFF);
                eps[2] = (byte)(steerTorqueRaw & 0xFF); eps[3] = (byte)((steerTorqueRaw >> 8) & 0xFF);
                eps[4] = (byte)((t / 500) & 0x01);
                eps[4] |= (byte)(((t / 80) & 0x07) << 1);
                _rx.Enqueue(BuildPacket(ch2, 0x120, 0x00, eps));
            }
            else
            {
                byte ch3 = (byte)((t + 3) % 4);
                ushort vRaw = (ushort)(3200 + (t % 800));
                short iRaw = (short)(-500 + (t % 1000));
                byte socRaw = (byte)(100 + (t % 120));
                byte sohRaw = 240;
                ushort insRaw = (ushort)(1500 + (t % 500));
                byte[] bms = new byte[8];
                bms[0] = (byte)(vRaw & 0xFF); bms[1] = (byte)(vRaw >> 8);
                bms[2] = (byte)(iRaw & 0xFF); bms[3] = (byte)((iRaw >> 8) & 0xFF);
                bms[4] = socRaw; bms[5] = sohRaw;
                bms[6] = (byte)(insRaw & 0xFF); bms[7] = (byte)(insRaw >> 8);
                _rx.Enqueue(BuildPacket(ch3, 0x140, 0x00, bms));
            }

            if ((t % 120) == 0)
            {
                var errCh = (byte)(t % 4);
                byte errCode = 0x5;
                byte errFlags = (byte)(0x04 | (errCode << 4));
                _rx.Enqueue(BuildPacket(errCh, 0x000, errFlags, new byte[0]));
            }
        }

        private static int ParseMockPeriod(string endpoint, int dft)
        {
            if (string.IsNullOrWhiteSpace(endpoint)) return dft;
            var idx = endpoint.IndexOf("period=", StringComparison.OrdinalIgnoreCase);
            if (idx < 0) return dft;
            var seg = endpoint.Substring(idx + 7);
            var end = seg.IndexOf('&');
            if (end >= 0) seg = seg.Substring(0, end);
            int val;
            if (int.TryParse(seg, out val))
            {
                if (val < 20) return 20;
                if (val > 2000) return 2000;
                return val;
            }
            return dft;
        }

        private static byte[] BuildPacket(byte channel, uint id, byte flags, byte[] data)
        {
            int dlc = data == null ? 0 : data.Length;
            var packet = new byte[8 + dlc];
            packet[0] = 0xA5;
            packet[1] = channel;
            packet[2] = (byte)dlc;
            packet[3] = flags;
            packet[4] = (byte)(id & 0xFF);
            packet[5] = (byte)((id >> 8) & 0xFF);
            packet[6] = (byte)((id >> 16) & 0xFF);
            packet[7] = (byte)((id >> 24) & 0xFF);
            if (dlc > 0) Buffer.BlockCopy(data, 0, packet, 8, dlc);
            return packet;
        }
    }

    public sealed class WinUsbTransport : ITransport
    {
        private static readonly Guid VbaCanInterfaceGuid = new Guid("620ABCCC-9C28-4D68-AA56-D7F27DE4B306");
        private const int PreferredVid = 0x1FC9;
        private const int PreferredPid = 0x0135;
        private readonly ConcurrentQueue<byte[]> _rx = new ConcurrentQueue<byte[]>();
        private readonly List<byte> _rxBuffer = new List<byte>(2048);

        private SafeFileHandle _deviceHandle;
        private IntPtr _winUsbHandle = IntPtr.Zero;
        private byte _bulkIn;
        private byte _bulkOut;
        private string _name = "WinUSB";

        public sealed class WinUsbDeviceInfo
        {
            public string DevicePath { get; set; }
            public int Vid { get; set; }
            public int Pid { get; set; }
        }

        public string Name { get { return _name; } }

        public static List<WinUsbDeviceInfo> EnumeratePresentDevices()
        {
            var result = new List<WinUsbDeviceInfo>();
            AppendDevicesForGuid(result, VbaCanInterfaceGuid);
            foreach (var guid in EnumerateRegistryInterfaceGuids())
            {
                AppendDevicesForGuid(result, guid);
            }
            AppendDevicesForGuid(result, Native.GUID_DEVINTERFACE_USB_DEVICE);
            return result;
        }

        private static IEnumerable<Guid> EnumerateRegistryInterfaceGuids()
        {
            var seen = new HashSet<Guid>();
            using (var usbRoot = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\USB"))
            {
                if (usbRoot == null)
                {
                    yield break;
                }

                foreach (var vidPidName in usbRoot.GetSubKeyNames())
                {
                    using (var vidPidKey = usbRoot.OpenSubKey(vidPidName))
                    {
                        if (vidPidKey == null)
                        {
                            continue;
                        }

                        foreach (var instanceName in vidPidKey.GetSubKeyNames())
                        {
                            using (var paramKey = vidPidKey.OpenSubKey(instanceName + @"\Device Parameters"))
                            {
                                if (paramKey == null)
                                {
                                    continue;
                                }

                                foreach (var guid in ReadInterfaceGuids(paramKey))
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
            var multi = paramKey.GetValue("DeviceInterfaceGUIDs") as string[];
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

            var single = paramKey.GetValue("DeviceInterfaceGUID") as string;
            Guid parsed;
            if (!string.IsNullOrWhiteSpace(single) && Guid.TryParse(single, out parsed))
            {
                yield return parsed;
            }
        }

        private static void AppendDevicesForGuid(List<WinUsbDeviceInfo> result, Guid guid)
        {
            var info = Native.SetupDiGetClassDevs(ref guid, IntPtr.Zero, IntPtr.Zero,
                Native.DIGCF_PRESENT | Native.DIGCF_DEVICEINTERFACE);
            if (info == IntPtr.Zero || info.ToInt64() == -1)
            {
                return;
            }

            try
            {
                uint index = 0;
                while (true)
                {
                    var ifData = new Native.SP_DEVICE_INTERFACE_DATA();
                    ifData.cbSize = Marshal.SizeOf(ifData);
                    if (!Native.SetupDiEnumDeviceInterfaces(info, IntPtr.Zero, ref guid, index, ref ifData))
                    {
                        break;
                    }

                    uint need;
                    Native.SetupDiGetDeviceInterfaceDetail(info, ref ifData, IntPtr.Zero, 0, out need, IntPtr.Zero);
                    var detail = Marshal.AllocHGlobal((int)need);
                    try
                    {
                        Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                        if (Native.SetupDiGetDeviceInterfaceDetail(info, ref ifData, detail, need, out need, IntPtr.Zero))
                        {
                            var pDevicePath = IntPtr.Add(detail, 4);
                            var path = Marshal.PtrToStringAuto(pDevicePath);
                            if (!string.IsNullOrWhiteSpace(path) &&
                                !result.Any(d => string.Equals(d.DevicePath, path, StringComparison.OrdinalIgnoreCase)))
                            {
                                int vid, pid;
                                ExtractVidPid(path, out vid, out pid);
                                result.Add(new WinUsbDeviceInfo
                                {
                                    DevicePath = path,
                                    Vid = vid,
                                    Pid = pid
                                });
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
        }

        public bool Open(string endpoint)
        {
            Close();
            try
            {
                int vid, pid;
                ParseWinUsbEndpoint(endpoint, out vid, out pid);
                var paths = FindWinUsbDevicePaths(vid, pid);
                if (paths.Count == 0)
                {
                    return false;
                }

                for (int i = 0; i < paths.Count; i++)
                {
                    var path = paths[i];
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

                    _name = "WinUSB";
                    return true;
                }

                Close();
                return false;
            }
            catch
            {
                Close();
                return false;
            }
        }

        public void Close()
        {
            var h = _winUsbHandle;
            _winUsbHandle = IntPtr.Zero;
            if (h != IntPtr.Zero)
            {
                Native.WinUsb_Free(h);
            }

            var dev = _deviceHandle;
            _deviceHandle = null;
            if (dev != null && !dev.IsInvalid)
            {
                dev.Close();
            }

            byte[] drop;
            while (_rx.TryDequeue(out drop)) { }
            _rxBuffer.Clear();
        }

        public bool Send(byte[] packet)
        {
            if (_winUsbHandle == IntPtr.Zero || packet == null)
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

        public bool TryReceive(out byte[] packet)
        {
            if (_rx.TryDequeue(out packet))
            {
                return true;
            }

            packet = null;
            if (_winUsbHandle == IntPtr.Zero)
            {
                return false;
            }

            var tmp = new byte[512];
            uint read;
            if (Native.WinUsb_ReadPipe(_winUsbHandle, _bulkIn, tmp, (uint)tmp.Length, out read, IntPtr.Zero))
            {
                if (read > 0)
                {
                    for (int i = 0; i < read; i++) _rxBuffer.Add(tmp[i]);
                    ParseRxFrames();
                }
            }

            return _rx.TryDequeue(out packet);
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

                int len = 8 + dlc;
                if (_rxBuffer.Count < len)
                {
                    break;
                }

                var frame = _rxBuffer.Take(len).ToArray();
                _rxBuffer.RemoveRange(0, len);
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

                if (pipe.PipeType == Native.USBD_PIPE_TYPE.UsbdPipeTypeBulk)
                {
                    if ((pipe.PipeId & 0x80) != 0)
                    {
                        if (inPipe == 0) inPipe = pipe.PipeId;
                    }
                    else
                    {
                        if (outPipe == 0) outPipe = pipe.PipeId;
                    }
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

        private static void ParseWinUsbEndpoint(string endpoint, out int vid, out int pid)
        {
            vid = -1;
            pid = -1;
            var s = (endpoint ?? "").Trim();
            var idx = s.IndexOf("://", StringComparison.Ordinal);
            var body = idx >= 0 ? s.Substring(idx + 3) : s;
            var qidx = body.IndexOf('?');
            var head = qidx >= 0 ? body.Substring(0, qidx) : body;
            var query = qidx >= 0 ? body.Substring(qidx + 1) : "";

            if (!string.IsNullOrWhiteSpace(head) && !string.Equals(head, "auto", StringComparison.OrdinalIgnoreCase))
            {
                var parts = head.Split(':');
                if (parts.Length == 2)
                {
                    vid = ParseInt(parts[0], -1);
                    pid = ParseInt(parts[1], -1);
                }
            }

            var kv = ParseQuery(query);
            string sv;
            if (kv.TryGetValue("vid", out sv)) vid = ParseInt(sv, vid);
            string sp;
            if (kv.TryGetValue("pid", out sp)) pid = ParseInt(sp, pid);
        }

        private static Dictionary<string, string> ParseQuery(string query)
        {
            var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            if (string.IsNullOrWhiteSpace(query)) return map;
            var parts = query.Split('&');
            for (int i = 0; i < parts.Length; i++)
            {
                var p = parts[i];
                var idx = p.IndexOf('=');
                if (idx <= 0) continue;
                map[p.Substring(0, idx)] = p.Substring(idx + 1);
            }
            return map;
        }

        private static int ParseInt(string text, int dft)
        {
            var s = (text ?? "").Trim();
            if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                int hv;
                if (int.TryParse(s.Substring(2), System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out hv))
                    return hv;
            }
            int v;
            if (int.TryParse(s, out v)) return v;
            return dft;
        }

        private static List<string> FindWinUsbDevicePaths(int vid, int pid)
        {
            var devices = EnumeratePresentDevices();
            var paths = new List<string>();

            if (vid >= 0 && pid >= 0)
            {
                for (int i = 0; i < devices.Count; i++)
                {
                    var d = devices[i];
                    if (d.Vid == vid && d.Pid == pid)
                    {
                        AppendUniquePath(paths, d.DevicePath);
                    }
                }
                return paths;
            }

            for (int i = 0; i < devices.Count; i++)
            {
                var d = devices[i];
                if (d.Vid == PreferredVid && d.Pid == PreferredPid)
                {
                    AppendUniquePath(paths, d.DevicePath);
                }
            }

            if (paths.Count > 0)
            {
                return paths;
            }

            if (devices.Count == 1)
            {
                AppendUniquePath(paths, devices[0].DevicePath);
            }

            return paths;
        }

        private static void AppendUniquePath(List<string> paths, string devicePath)
        {
            if (string.IsNullOrWhiteSpace(devicePath))
            {
                return;
            }

            if (!paths.Any(existing => string.Equals(existing, devicePath, StringComparison.OrdinalIgnoreCase)))
            {
                paths.Add(devicePath);
            }
        }

        private static void ExtractVidPid(string path, out int vid, out int pid)
        {
            vid = -1;
            pid = -1;
            if (string.IsNullOrWhiteSpace(path))
            {
                return;
            }

            var lp = path.ToLowerInvariant();
            var vi = lp.IndexOf("vid_");
            if (vi >= 0 && lp.Length >= vi + 8)
            {
                int v;
                if (int.TryParse(lp.Substring(vi + 4, 4), System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out v))
                {
                    vid = v;
                }
            }
            var pi = lp.IndexOf("pid_");
            if (pi >= 0 && lp.Length >= pi + 8)
            {
                int p;
                if (int.TryParse(lp.Substring(pi + 4, 4), System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out p))
                {
                    pid = p;
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

            public static readonly Guid GUID_DEVINTERFACE_USB_DEVICE = new Guid("A5DCBF10-6530-11D2-901F-00C04FB951ED");

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
            public static extern IntPtr SetupDiGetClassDevs(ref Guid ClassGuid, IntPtr Enumerator, IntPtr hwndParent, uint Flags);

            [DllImport("setupapi.dll", SetLastError = true)]
            public static extern bool SetupDiEnumDeviceInterfaces(IntPtr DeviceInfoSet, IntPtr DeviceInfoData, ref Guid InterfaceClassGuid, uint MemberIndex, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData);

            [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Auto)]
            public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr DeviceInfoSet, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
                IntPtr DeviceInterfaceDetailData, uint DeviceInterfaceDetailDataSize, out uint RequiredSize, IntPtr DeviceInfoData);

            [DllImport("setupapi.dll", SetLastError = true)]
            public static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

            [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
            public static extern SafeFileHandle CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
                IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_Initialize(SafeFileHandle DeviceHandle, out IntPtr InterfaceHandle);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_Free(IntPtr InterfaceHandle);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_QueryInterfaceSettings(IntPtr InterfaceHandle, byte AlternateSettingNumber, out USB_INTERFACE_DESCRIPTOR UsbAltInterfaceDescriptor);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_QueryPipe(IntPtr InterfaceHandle, byte AlternateInterfaceNumber, byte PipeIndex, out WINUSB_PIPE_INFORMATION PipeInformation);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_SetPipePolicy(IntPtr InterfaceHandle, byte PipeID, uint PolicyType, uint ValueLength, ref uint Value);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_ReadPipe(IntPtr InterfaceHandle, byte PipeID, byte[] Buffer, uint BufferLength, out uint LengthTransferred, IntPtr Overlapped);

            [DllImport("winusb.dll", SetLastError = true)]
            public static extern bool WinUsb_WritePipe(IntPtr InterfaceHandle, byte PipeID, byte[] Buffer, uint BufferLength, out uint LengthTransferred, IntPtr Overlapped);
        }
    }
}
