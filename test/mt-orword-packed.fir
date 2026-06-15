FIRRTL version 3.3.0
circuit MtOrWordPacked :
  module MtOrWordPacked :
    input clock : Clock
    input reset : UInt<1>
    input io_in : UInt<8>
    output io_out0 : UInt<8>
    output io_out1 : UInt<8>
    output io_out2 : UInt<8>
    output io_out3 : UInt<8>
    output io_out4 : UInt<8>
    output io_out5 : UInt<8>
    output io_out6 : UInt<8>
    output io_out7 : UInt<8>
    output io_out8 : UInt<8>
    output io_out9 : UInt<8>
    output io_out10 : UInt<8>
    output io_out11 : UInt<8>
    output io_out12 : UInt<8>
    output io_out13 : UInt<8>
    output io_out14 : UInt<8>
    output io_out15 : UInt<8>
    output io_out16 : UInt<8>
    output io_out17 : UInt<8>
    output io_out18 : UInt<8>
    output io_out19 : UInt<8>

    node shared = xor(io_in, UInt<8>(0h5a))
    regreset r0 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r1 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r2 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r3 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r4 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r5 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r6 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r7 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r8 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r9 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r10 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r11 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r12 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r13 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r14 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r15 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r16 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r17 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r18 : UInt<8>, clock, reset, UInt<8>(0h00)
    regreset r19 : UInt<8>, clock, reset, UInt<8>(0h00)
    node n0 = xor(shared, UInt<8>(0h0))
    node n1 = xor(shared, UInt<8>(0h1))
    node n2 = xor(shared, UInt<8>(0h2))
    node n3 = xor(shared, UInt<8>(0h3))
    node n4 = xor(shared, UInt<8>(0h4))
    node n5 = xor(shared, UInt<8>(0h5))
    node n6 = xor(shared, UInt<8>(0h6))
    node n7 = xor(shared, UInt<8>(0h7))
    node n8 = xor(shared, UInt<8>(0h8))
    node n9 = xor(shared, UInt<8>(0h9))
    node n10 = xor(shared, UInt<8>(0ha))
    node n11 = xor(shared, UInt<8>(0hb))
    node n12 = xor(shared, UInt<8>(0hc))
    node n13 = xor(shared, UInt<8>(0hd))
    node n14 = xor(shared, UInt<8>(0he))
    node n15 = xor(shared, UInt<8>(0hf))
    node n16 = xor(shared, UInt<8>(0h10))
    node n17 = xor(shared, UInt<8>(0h11))
    node n18 = xor(shared, UInt<8>(0h12))
    node n19 = xor(shared, UInt<8>(0h13))
    connect r0, n0
    connect r1, n1
    connect r2, n2
    connect r3, n3
    connect r4, n4
    connect r5, n5
    connect r6, n6
    connect r7, n7
    connect r8, n8
    connect r9, n9
    connect r10, n10
    connect r11, n11
    connect r12, n12
    connect r13, n13
    connect r14, n14
    connect r15, n15
    connect r16, n16
    connect r17, n17
    connect r18, n18
    connect r19, n19
    connect io_out0, r0
    connect io_out1, r1
    connect io_out2, r2
    connect io_out3, r3
    connect io_out4, r4
    connect io_out5, r5
    connect io_out6, r6
    connect io_out7, r7
    connect io_out8, r8
    connect io_out9, r9
    connect io_out10, r10
    connect io_out11, r11
    connect io_out12, r12
    connect io_out13, r13
    connect io_out14, r14
    connect io_out15, r15
    connect io_out16, r16
    connect io_out17, r17
    connect io_out18, r18
    connect io_out19, r19
