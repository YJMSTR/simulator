FIRRTL version 3.3.0
circuit MtMulticlock :
  module MtMulticlock :
    input clock : Clock
    input clock2 : Clock
    input reset : UInt<1>
    input io_in : UInt<1>
    input io_sel : UInt<1>
    output io_a : UInt<1>
    output io_b : UInt<1>
    output io_mix : UInt<1>

    regreset a : UInt<1>, clock, reset, UInt<1>(0h0)
    regreset b : UInt<1>, clock2, reset, UInt<1>(0h0)
    connect a, io_in
    connect b, a
    node mix = mux(io_sel, a, b)
    connect io_a, a
    connect io_b, b
    connect io_mix, mix
