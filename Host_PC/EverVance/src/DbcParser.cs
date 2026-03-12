using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;

namespace EverVance
{
    public static class DbcParser
    {
        private static readonly Regex MsgRegex = new Regex(@"^BO_\s+(\d+)\s+(\w+)\s*:\s*(\d+)\s+", RegexOptions.Compiled);
        private static readonly Regex SigRegex = new Regex(
            @"^SG_\s+(\w+)\s*:\s*(\d+)\|(\d+)@([01])([+-])\s*\(([-\d\.eE]+),([-\d\.eE]+)\)\s*\[([-\d\.eE]+)\|([-\d\.eE]+)\]\s*\""([^\""]*)\""",
            RegexOptions.Compiled);

        public static Dictionary<uint, CanMessageDefinition> Parse(string path)
        {
            var map = new Dictionary<uint, CanMessageDefinition>();
            CanMessageDefinition current = null;

            foreach (var raw in File.ReadLines(path))
            {
                var line = raw.Trim();
                if (line.Length == 0)
                {
                    continue;
                }

                var mm = MsgRegex.Match(line);
                if (mm.Success)
                {
                    var id = uint.Parse(mm.Groups[1].Value, CultureInfo.InvariantCulture);
                    var msg = new CanMessageDefinition
                    {
                        Id = id,
                        Name = mm.Groups[2].Value,
                        Dlc = int.Parse(mm.Groups[3].Value, CultureInfo.InvariantCulture)
                    };
                    map[id] = msg;
                    current = msg;
                    continue;
                }

                var sm = SigRegex.Match(line);
                if (sm.Success && current != null)
                {
                    current.Signals.Add(new SignalDefinition
                    {
                        Name = sm.Groups[1].Value,
                        StartBit = int.Parse(sm.Groups[2].Value, CultureInfo.InvariantCulture),
                        Length = int.Parse(sm.Groups[3].Value, CultureInfo.InvariantCulture),
                        IsLittleEndian = sm.Groups[4].Value == "1",
                        IsSigned = sm.Groups[5].Value == "-",
                        Factor = double.Parse(sm.Groups[6].Value, CultureInfo.InvariantCulture),
                        Offset = double.Parse(sm.Groups[7].Value, CultureInfo.InvariantCulture),
                        Min = double.Parse(sm.Groups[8].Value, CultureInfo.InvariantCulture),
                        Max = double.Parse(sm.Groups[9].Value, CultureInfo.InvariantCulture),
                        Unit = sm.Groups[10].Value
                    });
                }
            }

            return map;
        }

        public static bool TryDecodeSignal(byte[] data, SignalDefinition sig, out double value)
        {
            value = 0;
            if (data == null || sig == null || sig.Length <= 0 || sig.Length > 64)
            {
                return false;
            }

            ulong raw = 0;
            if (sig.IsLittleEndian)
            {
                ulong payload = 0;
                var count = Math.Min(data.Length, 8);
                for (var i = 0; i < count; i++)
                {
                    payload |= ((ulong)data[i]) << (8 * i);
                }
                raw = (payload >> sig.StartBit) & ((sig.Length == 64) ? ulong.MaxValue : ((1UL << sig.Length) - 1UL));
            }
            else
            {
                // 当前版本先支持 Intel 小端信号，Motorola 信号先返回失败。
                return false;
            }

            double signedOrUnsigned;
            if (sig.IsSigned)
            {
                var signBit = 1UL << (sig.Length - 1);
                if ((raw & signBit) != 0)
                {
                    var mask = (sig.Length == 64) ? ulong.MaxValue : ((1UL << sig.Length) - 1UL);
                    var twos = (~raw + 1UL) & mask;
                    signedOrUnsigned = -(double)twos;
                }
                else
                {
                    signedOrUnsigned = raw;
                }
            }
            else
            {
                signedOrUnsigned = raw;
            }

            value = signedOrUnsigned * sig.Factor + sig.Offset;
            return true;
        }
    }
}
