FIRRTL version 3.3.0
circuit MtCoarseBalancedMTasks :
  module MtCoarseBalancedMTasks :
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
    input io_a6 : UInt<8>
    input io_b6 : UInt<8>
    input io_g6 : UInt<1>
    input io_a7 : UInt<8>
    input io_b7 : UInt<8>
    input io_g7 : UInt<1>
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
    output io_x6 : UInt<8>
    output io_y6 : UInt<8>
    output io_z6 : UInt<8>
    output io_x7 : UInt<8>
    output io_y7 : UInt<8>
    output io_z7 : UInt<8>

    node a0_inc = add(io_a0, UInt<1>(0h1))
    node a0_next = tail(a0_inc, 1)
    node b0_mix = xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(io_b0, UInt<8>(0h00)), UInt<8>(0h07)), UInt<8>(0h0e)), UInt<8>(0h15)), UInt<8>(0h1c)), UInt<8>(0h23)), UInt<8>(0h2a)), UInt<8>(0h31)), UInt<8>(0h38)), UInt<8>(0h3f)), UInt<8>(0h46)), UInt<8>(0h4d)), UInt<8>(0h54)), UInt<8>(0h5b)), UInt<8>(0h62)), UInt<8>(0h69)), UInt<8>(0h70)), UInt<8>(0h77)), UInt<8>(0h7e)), UInt<8>(0h85)), UInt<8>(0h8c)), UInt<8>(0h93)), UInt<8>(0h9a)), UInt<8>(0ha1)), UInt<8>(0ha8)), UInt<8>(0haf)), UInt<8>(0hb6)), UInt<8>(0hbd))
    node x0_val = and(a0_next, b0_mix)
    node y0_val = or(a0_next, b0_mix)
    node z0_val = mux(io_g0, x0_val, y0_val)
    connect io_x0, x0_val
    connect io_y0, y0_val
    connect io_z0, z0_val

    node a1_inc = add(io_a1, UInt<1>(0h1))
    node a1_next = tail(a1_inc, 1)
    node b1_mix = xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(io_b1, UInt<8>(0h1d)), UInt<8>(0h24)), UInt<8>(0h2b)), UInt<8>(0h32)), UInt<8>(0h39)), UInt<8>(0h40)), UInt<8>(0h47)), UInt<8>(0h4e)), UInt<8>(0h55)), UInt<8>(0h5c)), UInt<8>(0h63)), UInt<8>(0h6a)), UInt<8>(0h71)), UInt<8>(0h78)), UInt<8>(0h7f)), UInt<8>(0h86)), UInt<8>(0h8d)), UInt<8>(0h94)), UInt<8>(0h9b)), UInt<8>(0ha2)), UInt<8>(0ha9)), UInt<8>(0hb0)), UInt<8>(0hb7)), UInt<8>(0hbe)), UInt<8>(0hc5)), UInt<8>(0hcc)), UInt<8>(0hd3)), UInt<8>(0hda))
    node x1_val = and(a1_next, b1_mix)
    node y1_val = or(a1_next, b1_mix)
    node z1_val = mux(io_g1, x1_val, y1_val)
    connect io_x1, x1_val
    connect io_y1, y1_val
    connect io_z1, z1_val

    node a2_inc = add(io_a2, UInt<1>(0h1))
    node a2_next = tail(a2_inc, 1)
    node b2_mix = xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(io_b2, UInt<8>(0h3a)), UInt<8>(0h41)), UInt<8>(0h48)), UInt<8>(0h4f)), UInt<8>(0h56)), UInt<8>(0h5d)), UInt<8>(0h64)), UInt<8>(0h6b)), UInt<8>(0h72)), UInt<8>(0h79)), UInt<8>(0h80)), UInt<8>(0h87)), UInt<8>(0h8e)), UInt<8>(0h95)), UInt<8>(0h9c)), UInt<8>(0ha3)), UInt<8>(0haa)), UInt<8>(0hb1)), UInt<8>(0hb8)), UInt<8>(0hbf)), UInt<8>(0hc6)), UInt<8>(0hcd)), UInt<8>(0hd4)), UInt<8>(0hdb)), UInt<8>(0he2)), UInt<8>(0he9)), UInt<8>(0hf0)), UInt<8>(0hf7))
    node x2_val = and(a2_next, b2_mix)
    node y2_val = or(a2_next, b2_mix)
    node z2_val = mux(io_g2, x2_val, y2_val)
    connect io_x2, x2_val
    connect io_y2, y2_val
    connect io_z2, z2_val

    node a3_inc = add(io_a3, UInt<1>(0h1))
    node a3_next = tail(a3_inc, 1)
    node b3_mix = xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(xor(io_b3, UInt<8>(0h57)), UInt<8>(0h5e)), UInt<8>(0h65)), UInt<8>(0h6c)), UInt<8>(0h73)), UInt<8>(0h7a)), UInt<8>(0h81)), UInt<8>(0h88)), UInt<8>(0h8f)), UInt<8>(0h96)), UInt<8>(0h9d)), UInt<8>(0ha4)), UInt<8>(0hab)), UInt<8>(0hb2)), UInt<8>(0hb9)), UInt<8>(0hc0)), UInt<8>(0hc7)), UInt<8>(0hce)), UInt<8>(0hd5)), UInt<8>(0hdc)), UInt<8>(0he3)), UInt<8>(0hea)), UInt<8>(0hf1)), UInt<8>(0hf8)), UInt<8>(0hff)), UInt<8>(0h06)), UInt<8>(0h0d)), UInt<8>(0h14))
    node x3_val = and(a3_next, b3_mix)
    node y3_val = or(a3_next, b3_mix)
    node z3_val = mux(io_g3, x3_val, y3_val)
    connect io_x3, x3_val
    connect io_y3, y3_val
    connect io_z3, z3_val

    node a4_inc = add(io_a4, UInt<1>(0h1))
    node a4_next = tail(a4_inc, 1)
    node b4_mix = xor(xor(xor(xor(xor(xor(xor(xor(io_b4, UInt<8>(0h74)), UInt<8>(0h7b)), UInt<8>(0h82)), UInt<8>(0h89)), UInt<8>(0h90)), UInt<8>(0h97)), UInt<8>(0h9e)), UInt<8>(0ha5))
    node x4_val = and(a4_next, b4_mix)
    node y4_val = or(a4_next, b4_mix)
    node z4_val = mux(io_g4, x4_val, y4_val)
    connect io_x4, x4_val
    connect io_y4, y4_val
    connect io_z4, z4_val

    node a5_inc = add(io_a5, UInt<1>(0h1))
    node a5_next = tail(a5_inc, 1)
    node b5_mix = xor(xor(xor(xor(xor(xor(xor(xor(io_b5, UInt<8>(0h91)), UInt<8>(0h98)), UInt<8>(0h9f)), UInt<8>(0ha6)), UInt<8>(0had)), UInt<8>(0hb4)), UInt<8>(0hbb)), UInt<8>(0hc2))
    node x5_val = and(a5_next, b5_mix)
    node y5_val = or(a5_next, b5_mix)
    node z5_val = mux(io_g5, x5_val, y5_val)
    connect io_x5, x5_val
    connect io_y5, y5_val
    connect io_z5, z5_val

    node a6_inc = add(io_a6, UInt<1>(0h1))
    node a6_next = tail(a6_inc, 1)
    node b6_mix = xor(xor(xor(xor(xor(xor(xor(xor(io_b6, UInt<8>(0hae)), UInt<8>(0hb5)), UInt<8>(0hbc)), UInt<8>(0hc3)), UInt<8>(0hca)), UInt<8>(0hd1)), UInt<8>(0hd8)), UInt<8>(0hdf))
    node x6_val = and(a6_next, b6_mix)
    node y6_val = or(a6_next, b6_mix)
    node z6_val = mux(io_g6, x6_val, y6_val)
    connect io_x6, x6_val
    connect io_y6, y6_val
    connect io_z6, z6_val

    node a7_inc = add(io_a7, UInt<1>(0h1))
    node a7_next = tail(a7_inc, 1)
    node b7_mix = xor(xor(xor(xor(xor(xor(xor(xor(io_b7, UInt<8>(0hcb)), UInt<8>(0hd2)), UInt<8>(0hd9)), UInt<8>(0he0)), UInt<8>(0he7)), UInt<8>(0hee)), UInt<8>(0hf5)), UInt<8>(0hfc))
    node x7_val = and(a7_next, b7_mix)
    node y7_val = or(a7_next, b7_mix)
    node z7_val = mux(io_g7, x7_val, y7_val)
    connect io_x7, x7_val
    connect io_y7, y7_val
    connect io_z7, z7_val
