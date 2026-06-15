FIRRTL version 3.3.0
circuit MtCoarseCrossWord :
  module MtCoarseCrossWord :
    input clock : Clock
    input reset : UInt<1>
    input io_a0 : UInt<8>
    input io_b0 : UInt<8>
    input io_g0 : UInt<1>
    input io_a1 : UInt<8>
    input io_b1 : UInt<8>
    input io_g1 : UInt<1>
    input io_a2 : UInt<8>
    input io_b2 : UInt<8>
    input io_g2 : UInt<1>
    input io_a3 : UInt<8>
    input io_b3 : UInt<8>
    input io_g3 : UInt<1>
    input io_a4 : UInt<8>
    input io_b4 : UInt<8>
    input io_g4 : UInt<1>
    input io_a5 : UInt<8>
    input io_b5 : UInt<8>
    input io_g5 : UInt<1>
    output io_x0 : UInt<8>
    output io_y0 : UInt<8>
    output io_z0 : UInt<8>
    output io_x1 : UInt<8>
    output io_y1 : UInt<8>
    output io_z1 : UInt<8>
    output io_x2 : UInt<8>
    output io_y2 : UInt<8>
    output io_z2 : UInt<8>
    output io_x3 : UInt<8>
    output io_y3 : UInt<8>
    output io_z3 : UInt<8>
    output io_x4 : UInt<8>
    output io_y4 : UInt<8>
    output io_z4 : UInt<8>
    output io_x5 : UInt<8>
    output io_y5 : UInt<8>
    output io_z5 : UInt<8>

    node a0_inc = add(io_a0, UInt<1>(0h1))
    node a0_next = tail(a0_inc, 1)
    node b0_mix = xor(io_b0, UInt<8>(0haa))
    node x0_val = and(a0_next, b0_mix)
    node y0_val = or(a0_next, b0_mix)
    node z0_val = mux(io_g0, x0_val, y0_val)
    connect io_x0, x0_val
    connect io_y0, y0_val
    connect io_z0, z0_val

    node a1_inc = add(io_a1, UInt<1>(0h1))
    node a1_next = tail(a1_inc, 1)
    node b1_mix = xor(io_b1, UInt<8>(0h99))
    node x1_val = and(a1_next, b1_mix)
    node y1_val = or(a1_next, b1_mix)
    node z1_val = mux(io_g1, x1_val, y1_val)
    connect io_x1, x1_val
    connect io_y1, y1_val
    connect io_z1, z1_val

    node a2_inc = add(io_a2, UInt<1>(0h1))
    node a2_next = tail(a2_inc, 1)
    node b2_mix = xor(io_b2, UInt<8>(0h88))
    node x2_val = and(a2_next, b2_mix)
    node y2_val = or(a2_next, b2_mix)
    node z2_val = mux(io_g2, x2_val, y2_val)
    connect io_x2, x2_val
    connect io_y2, y2_val
    connect io_z2, z2_val

    node a3_inc = add(io_a3, UInt<1>(0h1))
    node a3_next = tail(a3_inc, 1)
    node b3_mix = xor(io_b3, UInt<8>(0h77))
    node x3_val = and(a3_next, b3_mix)
    node y3_val = or(a3_next, b3_mix)
    node z3_val = mux(io_g3, x3_val, y3_val)
    connect io_x3, x3_val
    connect io_y3, y3_val
    connect io_z3, z3_val

    node a4_inc = add(io_a4, UInt<1>(0h1))
    node a4_next = tail(a4_inc, 1)
    node b4_mix = xor(io_b4, UInt<8>(0h66))
    node x4_val = and(a4_next, b4_mix)
    node y4_val = or(a4_next, b4_mix)
    node z4_val = mux(io_g4, x4_val, y4_val)
    connect io_x4, x4_val
    connect io_y4, y4_val
    connect io_z4, z4_val

    node a5_inc = add(io_a5, UInt<1>(0h1))
    node a5_next = tail(a5_inc, 1)
    node b5_mix = xor(io_b5, UInt<8>(0h55))
    node x5_val = and(a5_next, b5_mix)
    node y5_val = or(a5_next, b5_mix)
    node z5_val = mux(io_g5, x5_val, y5_val)
    connect io_x5, x5_val
    connect io_y5, y5_val
    connect io_z5, z5_val
