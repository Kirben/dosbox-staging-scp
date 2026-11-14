# 8088 instruction compatibility

Use this optional CPU setting for early PC booters that require 8088 instruction
behavior:

```ini
[cpu]
cputype = 8088
core = normal
cpu_cycles = 1000
```

Selecting `8088` forces the normal interpreter, including when `core = auto` or a
dynamic core was configured. Other CPU types retain their existing behavior.
The cycle setting above is an example used for Lode Runner, not a conversion to
the clock rate of a physical 8088.

This mode implements single-byte `POP CS`, the original `PUSH SP` value, fixed
reserved FLAGS bits, unmasked CL shift counts, 16-bit instruction-fetch wrapping,
and the early conditional-jump, return, and LOCK opcode aliases. `POP CS` updates
the code segment without changing the next instruction offset. It is enabled
only for this CPU type: later processors must continue decoding `0Fh` as an
extended-opcode prefix.

This is an initial instruction compatibility mode. It is not a complete or
cycle-accurate 8088 model. It does not reproduce the four-byte prefetch queue,
eight-bit bus transactions, instruction-specific cycle counts, every undocumented
opcode/flag result, or all original exception and segment-boundary quirks.
Programs depending on those behaviors still need further CPU work.

Lode Runner's original 160K PC-booter TransCopy capture reaches its first level
with this mode without modifying guest RAM or disk contents. TransCopy mounting
requires the separate floppy-image support; the CPU mode itself is independent
of any disk format.

The synthetic `Cpu8088Test` tests exercise the CPU instructions without game
data, including the segment-restore sequence used by Lode Runner, changing CS,
pointer wrapping, CPU configuration, and later-CPU controls.

Hardware behavior references:

- [Intel 8086/8088 CPU information maintained by PCjs](https://www.pcjs.org/documents/manuals/intel/8086/)
- [8088 hardware instruction tests](https://github.com/SingleStepTests/8088)

The implementation extends Staging's existing interpreter; no DOSBox-X CPU code
was copied.
