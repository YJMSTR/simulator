FIRRTL version 3.3.0
circuit MtComb :
  module MtComb :
    input clock : Clock
    input reset : UInt<1>
    input io_a : UInt<8>
    input io_b : UInt<8>
    input io_gate : UInt<1>
    output io_x : UInt<8>
    output io_y : UInt<8>
    output io_z : UInt<8>

    node a_inc = add(io_a, UInt<1>(0h1))
    node a_next = tail(a_inc, 1)
    node b_mix = xor(io_b, UInt<8>(0haa))
    node x_val = and(a_next, b_mix)
    node y_val = or(a_next, b_mix)
    node z_val = mux(io_gate, x_val, y_val)
    connect io_x, x_val
    connect io_y, y_val
    connect io_z, z_val
