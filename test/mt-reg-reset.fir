FIRRTL version 3.3.0
circuit MtRegReset :
  module MtRegReset :
    input clock : Clock
    input reset : UInt<1>
    input io_in : UInt<8>
    input io_en : UInt<1>
    output io_out : UInt<8>

    regreset state : UInt<8>, clock, reset, UInt<8>(0h0)
    node add_state = add(state, io_in)
    node next_state = tail(add_state, 1)
    when io_en :
      connect state, next_state
    connect io_out, state
