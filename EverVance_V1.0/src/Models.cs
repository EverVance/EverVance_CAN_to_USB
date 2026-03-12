using System;
using System.Collections.Generic;

namespace EverVance
{
    public sealed class SignalDefinition
    {
        public string Name { get; set; }
        public int StartBit { get; set; }
        public int Length { get; set; }
        public bool IsLittleEndian { get; set; }
        public bool IsSigned { get; set; }
        public double Factor { get; set; }
        public double Offset { get; set; }
        public double Min { get; set; }
        public double Max { get; set; }
        public string Unit { get; set; }
    }

    public sealed class CanMessageDefinition
    {
        public uint Id { get; set; }
        public string Name { get; set; }
        public int Dlc { get; set; }
        public List<SignalDefinition> Signals { get; private set; }

        public CanMessageDefinition()
        {
            Signals = new List<SignalDefinition>();
        }

        public override string ToString()
        {
            return string.Format("0x{0:X3} {1}", Id, Name);
        }
    }

    public sealed class CanFrame
    {
        public DateTime Timestamp { get; set; }
        public int Channel { get; set; }
        public bool IsCanFd { get; set; }
        public bool IsTx { get; set; }
        public bool HasError { get; set; }
        public byte ErrorCode { get; set; }
        public string ErrorType { get; set; }
        public uint Id { get; set; }
        public byte[] Data { get; set; }
    }

    public sealed class SignalSample
    {
        public DateTime Timestamp { get; set; }
        public int Channel { get; set; }
        public string Message { get; set; }
        public string Signal { get; set; }
        public double Value { get; set; }
    }
}
