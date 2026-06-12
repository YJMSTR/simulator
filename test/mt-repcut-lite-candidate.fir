FIRRTL version 3.3.0
circuit MtRepCutLiteCandidate :
  module MtRepCutLiteCandidate :
    input clock : Clock
    input reset : UInt<1>
    input io_a : UInt<8>
    input io_b : UInt<8>
    output io_x : UInt<8>
    output io_y : UInt<8>

    node a_mix = xor(io_a, UInt<8>(0haa))
    node x_val = add(a_mix, UInt<1>(0h1))
    node x_tail = tail(x_val, 1)
    node b_mix = xor(io_b, UInt<8>(0h55))
    node y_val = add(b_mix, UInt<1>(0h3))
    node y_tail = tail(y_val, 1)
    connect io_x, x_tail
    connect io_y, y_tail
