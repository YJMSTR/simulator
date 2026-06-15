FIRRTL version 3.3.0
circuit MtStateUpdateProofGate :
  module MtStateUpdateProofGate :
    input clock : Clock
    input reset : UInt<1>
    input io_a : UInt<8>
    input io_b : UInt<8>
    input io_sel : UInt<1>
    output io_x : UInt<8>
    output io_y : UInt<8>
    output io_z : UInt<8>
    output io_chain : UInt<8>
    output io_d : UInt<8>
    output io_unique : UInt<8>
    output io_dup : UInt<8>

    reg r0 : UInt<8>, clock
    reg r1 : UInt<8>, clock
    regreset rr : UInt<8>, clock, reset, UInt<8>(0h0)
    reg chain_a : UInt<8>, clock
    reg chain_b : UInt<8>, clock
    reg d_only : UInt<8>, clock
    reg unique_only : UInt<8>, clock
    reg dup_writer : UInt<8>, clock

    node r0_inc = add(r0, io_a)
    node r0_next = tail(r0_inc, 1)
    node r1_xor = xor(r1, io_b)
    node rr_mix = mux(io_sel, r0, io_b)
    node chain_a_inc = add(chain_a, io_a)
    node chain_a_next = tail(chain_a_inc, 1)

    connect r0, r0_next
    connect r1, r1_xor
    connect chain_a, chain_a_next
    connect chain_b, chain_a
    connect d_only, io_b
    connect unique_only, io_a
    connect dup_writer, io_a
    when io_sel :
      connect rr, rr_mix
    when io_sel :
      connect dup_writer, io_b

    node fanout_sum = add(r0, r1)
    node fanout_next = tail(fanout_sum, 1)
    connect io_x, r0
    connect io_y, fanout_next
    connect io_z, rr
    connect io_chain, chain_b
    connect io_d, d_only
    connect io_unique, unique_only
    connect io_dup, dup_writer
