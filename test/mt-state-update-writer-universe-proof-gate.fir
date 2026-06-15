FIRRTL version 3.3.0
circuit MtStateUpdateWriterUniverseProofGate :
  module MtStateUpdateWriterUniverseProofGate :
    input clock : Clock
    input io_a : UInt<8>
    input io_b : UInt<8>
    input io_sel : UInt<1>
    output io_overlap : UInt<8>
    output io_other : UInt<8>
    output io_unique : UInt<8>

    reg overlap : UInt<8>, clock
    reg other : UInt<8>, clock
    reg unique_local : UInt<8>, clock

    node overlap_inc = add(overlap, io_a)
    node overlap_next = tail(overlap_inc, 1)
    node other_inc = add(other, io_b)
    node other_next = tail(other_inc, 1)

    connect overlap, io_a
    connect unique_local, io_b
    when io_sel :
      connect overlap, overlap_next
      connect other, other_next

    connect io_overlap, overlap
    connect io_other, other
    connect io_unique, unique_local
