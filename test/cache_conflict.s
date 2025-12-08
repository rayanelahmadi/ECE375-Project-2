_start:
    # Sequence of loads mapping to the same D-cache set to exercise LRU
    # D$ config from cache_config.txt: size=4096, block=16, ways=4 => sets=64
    # Addresses spaced by 64*blockSize = 1024 map to the same set.
    # Access pattern:
    #   0       (miss)
    #   1024    (miss)
    #   2048    (miss)
    #   3072    (miss)   ; set now full (4 ways)
    #   0       (hit)    ; reuse updates LRU
    #   4096    (miss)   ; conflicts, evicts true LRU

    # Load from 0
    addi t0, x0, 0
    ld   t1, 0(t0)

    # Load from 1024
    addi t0, t0, 1024
    ld   t1, 0(t0)

    # Load from 2048
    addi t0, t0, 1024
    ld   t1, 0(t0)

    # Load from 3072
    addi t0, t0, 1024
    ld   t1, 0(t0)

    # Reuse 0 (should be a hit)
    addi t0, x0, 0
    ld   t1, 0(t0)

    # Load from 4096 (new tag in same set; should miss and evict true LRU)
    lui  t0, 0x1         # t0 = 0x1000 (4096)
    ld   t1, 0(t0)

    # Halt
    .word 0xfeedfeed


