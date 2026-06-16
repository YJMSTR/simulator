FIRRTL version 3.3.0
circuit Mt22xBlockers :
  extmodule BlackBox :
    input in : UInt<8>
    output out : UInt<8>

  module Mt22xBlockers :
    input clock : Clock
    input reset : UInt<1>
    input io_addr : UInt<2>
    input io_wdata : UInt<8>
    input io_we : UInt<1>
    input io_in : UInt<8>
    output io_mem : UInt<8>
    output io_reg : UInt<8>
    output io_ext : UInt<8>

    smem mem : UInt<8>[4]
    read mport r = mem[io_addr], clock
    write mport w = mem[io_addr], clock
    when io_we :
      connect w, io_wdata
    connect io_mem, r

    regreset state : UInt<8>, clock, reset, UInt<8>(0h0)
    node next_sum = add(state, io_in)
    node next_state = tail(next_sum, 1)
    when io_we :
      connect state, next_state
    connect io_reg, state

    inst bb of BlackBox
    connect bb.in, io_in
    connect io_ext, bb.out
