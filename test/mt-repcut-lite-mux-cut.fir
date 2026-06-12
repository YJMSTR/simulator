FIRRTL version 3.3.0
circuit MtRepCutLiteMuxCut :
  module MtRepCutLiteMuxCut :
    input clock : Clock
    input reset : UInt<1>
    input io_sel : UInt<1>
    input io_a : UInt<8>
    input io_b : UInt<8>
    input io_c : UInt<8>
    output io_x : UInt<8>
    output io_y : UInt<8>

    node chosen = mux(io_sel, io_a, io_b)
    node c_mix = xor(io_c, UInt<8>(0h5a))
    node x_val = and(chosen, c_mix)
    node y_val = or(chosen, c_mix)
    connect io_x, x_val
    connect io_y, y_val
