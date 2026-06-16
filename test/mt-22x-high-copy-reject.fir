FIRRTL version 3.3.0
circuit Mt22xHighCopyReject :
  module Mt22xHighCopyReject :
    input clock : Clock
    input reset : UInt<1>
    input io_a : UInt<8>
    input io_b : UInt<8>
    output io_x : UInt<8>
    output io_y : UInt<8>

    node s0 = add(io_a, UInt<1>(0h1))
    node s1 = tail(s0, 1)
    node s2 = xor(s1, UInt<8>(0h11))
    node s3 = add(s2, io_b)
    node s4 = tail(s3, 1)
    node s5 = xor(s4, UInt<8>(0h22))
    node s6 = add(s5, io_a)
    node s7 = tail(s6, 1)
    node s8 = xor(s7, UInt<8>(0h33))
    node s9 = add(s8, io_b)
    node s10 = tail(s9, 1)
    node s11 = xor(s10, UInt<8>(0h44))
    node s12 = add(s11, io_a)
    node s13 = tail(s12, 1)
    node s14 = xor(s13, UInt<8>(0h55))
    node s15 = add(s14, io_b)
    node shared = tail(s15, 1)
    node x_val = and(shared, io_a)
    node y_val = or(shared, io_b)
    connect io_x, x_val
    connect io_y, y_val
