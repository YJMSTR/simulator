FIRRTL version 3.3.0
circuit Mt22xActivitySkew :
  module Mt22xActivitySkew :
    input clock : Clock
    input reset : UInt<1>
    input io_hot_a : UInt<8>
    input io_hot_b : UInt<8>
    input io_cold : UInt<8>
    input io_g0 : UInt<1>
    input io_g1 : UInt<1>
    output io_hot0 : UInt<8>
    output io_hot1 : UInt<8>
    output io_cold_out : UInt<8>

    node h0 = add(io_hot_a, io_hot_b)
    node h1 = tail(h0, 1)
    node h2 = xor(h1, UInt<8>(0haa))
    node hot0_val = mux(io_g0, h2, io_hot_a)
    node hot1_base = add(h2, io_hot_b)
    node hot1_val = tail(hot1_base, 1)

    node c0 = add(io_cold, UInt<1>(0h1))
    node c1 = tail(c0, 1)
    node c2 = xor(c1, UInt<8>(0h11))
    node c3 = add(c2, UInt<8>(0h22))
    node c4 = tail(c3, 1)
    node c5 = xor(c4, UInt<8>(0h33))
    node c6 = add(c5, UInt<8>(0h44))
    node c7 = tail(c6, 1)
    node cold_val = mux(io_g1, c7, io_cold)

    connect io_hot0, hot0_val
    connect io_hot1, hot1_val
    connect io_cold_out, cold_val
