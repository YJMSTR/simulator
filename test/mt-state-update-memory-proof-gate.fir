FIRRTL version 3.3.0
circuit MtStateUpdateMemoryProofGate :
  module MtStateUpdateMemoryProofGate :
    input clock : Clock
    input io_addr : UInt<2>
    input io_wdata : UInt<8>
    input io_we : UInt<1>
    output io_rdata : UInt<8>

    smem mem : UInt<8>[4]
    read mport r = mem[io_addr], clock
    write mport w = mem[io_addr], clock
    when io_we :
      connect w, io_wdata
    connect io_rdata, r
