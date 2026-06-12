FIRRTL version 3.3.0
circuit MtAsyncReset :
  module MtAsyncReset :
    input clock : Clock
    input io_reset : UInt<1>
    input io_gate : UInt<1>
    input io_in : UInt<1>
    output io_out : UInt<1>

    node reset_en = and(io_reset, io_gate)
    node async_reset = asAsyncReset(reset_en)
    regreset state : UInt<1>, clock, async_reset, UInt<1>(0h0)
    connect state, io_in
    connect io_out, state
