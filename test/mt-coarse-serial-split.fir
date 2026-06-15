FIRRTL version 3.3.0
circuit MtCoarseSerialSplit :
  module MtCoarseSerialSplit :
    input clock : Clock
    input reset : UInt<1>
    input io_a : UInt<8>
    input io_b : UInt<8>
    input io_c : UInt<8>
    input io_g : UInt<1>
    output io_x : UInt<8>
    output io_y : UInt<8>
    output io_z : UInt<8>

    regreset r : UInt<8>, clock, reset, UInt<8>(0h0)

    node a_inc = add(io_a, UInt<1>(0h1))
    node a_next = tail(a_inc, 1)
    node b_mix = xor(io_b, UInt<8>(0haa))
    node x_val = and(a_next, b_mix)
    connect io_x, x_val

    node r_next = add(r, io_c)
    node r_mux = mux(io_g, r_next, r)
    connect r, r_mux

    node y_val = or(io_c, UInt<8>(0h11))
    node z_val = xor(y_val, r)
    connect io_y, y_val
    connect io_z, z_val
