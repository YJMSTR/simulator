FIRRTL version 3.3.0
circuit ReproAsyncReset :
  module ReproAsyncReset :
    input clock : Clock
    input reset : AsyncReset
    input io_in : UInt<1>
    output io_out : UInt<1>

    regreset state : UInt<1>, clock, reset, UInt<1>(0h0)
    connect state, io_in
    connect io_out, state
