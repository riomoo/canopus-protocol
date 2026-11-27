// Package arm decodes the ARM (A32) and Thumb instructions of an ARMv6K +
// VFPv2 program, as armcc emits them for the 3DS application core.
package arm

// Kind is the instruction class the code generator switches on.
Kind :: enum int {
	Unknown,
	DataProc,   // AND..MVN: Op, S, Rd, Rn, Op2
	Mul,        // MUL/MLA/MLS (Op: MUL, MLA): Rd, Rn(=acc), Rm, Rs, S
	MulLong,    // UMULL/UMLAL/SMULL/SMLAL/UMAAL: RdLo(Rd), RdHi(Rn), Rm, Rs, S, Op
	MulHalf,    // SMLAxy/SMULxy/SMULWy/SMLAWy/SMLALxy: Op, Rd, Rn(acc), Rm, Rs, X, Y
	MulDual,    // SMUAD/SMLAD/.../USAD8/USADA8: Op, Rd, Rn(acc), Rm, Rs, X
	Clz,        // Rd, Rm
	Sat,        // QADD/QSUB/QDADD/QDSUB: Op, Rd, Rm, Rn
	Mrs,        // Rd
	Msr,        // Mask, Op2 (imm or reg)
	Hint,       // NOP/YIELD/WFE/WFI/SEV and other no-op hints
	Branch,     // B/BL: Link, Target
	BlxImm,     // BLX imm (to Thumb): Target
	BranchReg,  // BX/BLX reg: Link, Rm
	Mem,        // LDR/STR word/byte/half/signed/double: Load, Size, Signed, Rd, Rn, Off, P, U, W
	MemEx,      // LDREX/STREX (B/H/D): Load, Size, Rd, Rn, Rm (STREX value)
	Swp,        // SWP/SWPB: Size, Rd, Rm, Rn
	Clrex,
	Block,      // LDM/STM: Load, Rn, RegList, P, U, W
	Svc,        // Imm
	Bkpt,       // Imm
	Extend,     // SXTB/SXTH/UXTB/UXTH/SXTAB/.../SXTB16/UXTB16: Op, Rd, Rn (15 = none), Rm, Rot
	Pkh,        // PKHBT/PKHTB: Op(0 BT, 1 TB), Rd, Rn, Rm, Shift
	Ssat,       // SSAT/USAT: Op(0 S, 1 U), Rd, SatImm, Rm, ShiftType, Shift
	Sat16,      // SSAT16/USAT16: Op, Rd, SatImm, Rm
	Rev,        // REV/REV16/REVSH: Op, Rd, Rm
	Sel,        // Rd, Rn, Rm
	Parallel,   // SADD16..UHSUB8: Op (prefix<<3 | op), Rd, Rn, Rm
	Pld,        // preload (no-op)
	Cps,        // CPS/SETEND (ignored in user mode)
	CoprocReg,  // MRC/MCR (non-VFP): Load(=MRC), Cp, Opc1, Rd, CRn, CRm, Opc2
	VfpArith,   // VFP data processing: Op, Dbl, Vd, Vn, Vm
	VfpCmp,     // VCMP/VCMPE (Zero: compare with 0.0): Dbl, Vd, Vm, E
	VfpCvt,     // conversions: Op, Vd, Vm
	VfpMovCore, // VMOV Sn <-> Rt: Load(=to core), Rd, Vn
	VfpMovHalf, // VMOV Dn[x] <-> Rt: Load, Rd, Vn, X
	VfpMov2,    // VMOV Rt,Rt2 <-> Sm,Sm+1 or Dm: Load, Dbl, Rd, Rn(=Rt2), Vm
	VfpSys,     // VMRS/VMSR: Load(=VMRS), Rd (15 = APSR_nzcv), Reg
	VfpMem,     // VLDR/VSTR: Load, Dbl, Vd, Rn, Imm, U
	VfpBlock,   // VLDM/VSTM: Load, Dbl, Vd, Count, Rn, P, U, W
}

// Shift types.
LSL :: 0
LSR :: 1
ASR :: 2
ROR :: 3
RRX :: 4

// Operand is a flexible second operand or a memory offset.
Operand :: struct {
	imm:       bool, // an immediate (value)
	value:     u32,  // immediate value
	rot:       u32,  // data-processing immediate rotation (carry out when non-zero)
	rm:        int,
	shift:     int,  // LSL/LSR/ASR/ROR/RRX
	amount:    u32,  // immediate shift amount (normalised: LSR/ASR #0 -> 32)
	reg_shift: bool, // shift by register rs
	rs:        int,
}

// Data-processing opcodes.
AND :: 0
EOR :: 1
SUB :: 2
RSB :: 3
ADD :: 4
ADC :: 5
SBC :: 6
RSC :: 7
TST :: 8
TEQ :: 9
CMP :: 10
CMN :: 11
ORR :: 12
MOV :: 13
BIC :: 14
MVN :: 15

DP_NAMES := [16]string{"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"}

// VFP arithmetic ops.
VMLA  :: 0
VMLS  :: 1
VNMLS :: 2
VNMLA :: 3
VMUL  :: 4
VNMUL :: 5
VADD  :: 6
VSUB  :: 7
VDIV  :: 8
VMOV  :: 9 // register move
VABS  :: 10
VNEG  :: 11
VSQRT :: 12

// VFP conversions.
CVT_F64_F32 :: 0 // Dd = (double)Sm
CVT_F32_F64 :: 1 // Sd = (float)Dm
CVT_F32_U32 :: 2
CVT_F32_S32 :: 3
CVT_F64_U32 :: 4
CVT_F64_S32 :: 5
CVT_U32_F32 :: 6 // round toward zero (E: FPSCR rounding)
CVT_S32_F32 :: 7
CVT_U32_F64 :: 8
CVT_S32_F64 :: 9

// Inst is one decoded instruction.
Inst :: struct {
	addr:     u32,
	raw:      u32,
	cond:     int, // 0..14, 14 = always
	kind:     Kind,
	op:       int,
	s:        bool,
	rd, rn:   int,
	rm, rs:   int,
	rd2:      int,
	op2:      Operand,
	// memory
	load, signed: bool,
	size:     int, // 1, 2, 4, 8
	p, u, w:  bool,
	reg_list: u16,
	off:      Operand,
	// misc
	link:     bool,
	target:   u32,
	imm:      u32,
	mask:     u32,
	x, y:     bool,
	e:        bool,
	zero:     bool,
	dbl:      bool,
	vd, vn:   int,
	vm:       int,
	count:    int,
	cp:       int,
	opc1:     int,
	opc2:     int,
	crn:      int,
	crm:      int,
	rot:      u32,
	shift_t:  int,
	shift:    u32,
	reg:      int,
	// Thumb
	thumb:    bool,
	len:      u32,  // 4 (ARM, Thumb BL pair) or 2
	align_pc: bool, // Thumb LDR literal / ADR: PC reads as Align(addr+4, 4)
	exchange: bool, // BLX imm: the target is in the other instruction set
}

// pc is the value a read of r15 gives in this instruction.
pc :: proc(in_: Inst) -> u32 {
	if !in_.thumb {
		return in_.addr + 8
	}
	if in_.align_pc {
		return (in_.addr + 4) &~ 3
	}
	return in_.addr + 4
}

@(private)
bits :: #force_inline proc(x: u32, hi, lo: uint) -> u32 {
	return (x >> lo) & ((u32(1) << (hi - lo + 1)) - 1)
}
@(private)
bit :: #force_inline proc(x: u32, n: uint) -> bool { return (x >> n) & 1 != 0 }
@(private)
ror :: proc(v, n: u32) -> u32 {
	n := n & 31
	return v >> n | v << ((32 - n) & 31)
}

@(private)
shifter_imm :: proc(raw: u32) -> Operand {
	return Operand{rm = int(bits(raw, 3, 0)), shift = shift_kind(bits(raw, 6, 5), bits(raw, 11, 7)), amount = shift_amt(bits(raw, 6, 5), bits(raw, 11, 7))}
}

@(private)
shift_kind :: proc(t, amt: u32) -> int {
	if t == ROR && amt == 0 {
		return RRX
	}
	return int(t)
}

@(private)
shift_amt :: proc(t, amt: u32) -> u32 {
	if (t == LSR || t == ASR) && amt == 0 {
		return 32
	}
	return amt
}

// decode decodes one ARM instruction at addr.
decode :: proc(addr, raw: u32) -> Inst {
	in_ := Inst{addr = addr, raw = raw, cond = int(raw >> 28), rn = -1, rd = -1, rm = -1, rs = -1, rd2 = -1, len = 4}
	if in_.cond == 15 {
		decode_uncond(&in_)
		return in_
	}
	switch bits(raw, 27, 25) {
	case 0:
		decode_000(&in_)
	case 1:
		decode_001(&in_)
	case 2:
		decode_mem(&in_, false)
	case 3:
		if bit(raw, 4) {
			decode_media(&in_)
		} else {
			decode_mem(&in_, true)
		}
	case 4:
		in_.kind = .Block
		in_.p, in_.u, in_.w, in_.load = bit(raw, 24), bit(raw, 23), bit(raw, 21), bit(raw, 20)
		in_.rn = int(bits(raw, 19, 16))
		in_.reg_list = u16(raw & 0xFFFF)
		if bit(raw, 22) { // user-bank / exception return forms
			in_.kind = .Unknown
		}
	case 5:
		in_.kind = .Branch
		in_.link = bit(raw, 24)
		off := i32(raw << 8) >> 6
		in_.target = addr + 8 + u32(off)
	case 6:
		decode_coproc_mem(&in_)
	case 7:
		if bit(raw, 24) {
			in_.kind = .Svc
			in_.imm = raw & 0xFFFFFF
		} else {
			decode_coproc(&in_)
		}
	}
	return in_
}

@(private)
decode_uncond :: proc(in_: ^Inst) {
	raw := in_.raw
	switch {
	case bits(raw, 27, 25) == 5: // BLX imm: a call into Thumb
		in_.kind = .Branch
		in_.link = true
		in_.exchange = true
		off := i32(raw << 8) >> 6
		in_.target = in_.addr + 8 + u32(off) + (bits(raw, 24, 24) << 1)
	case raw & 0x0D70F000 == 0x0550F000: // PLD
		in_.kind = .Pld
	case raw == 0xF57FF01F:
		in_.kind = .Clrex
	case raw & 0x0FF10020 == 0x01000000 || raw & 0x0FFFFDFF == 0x01010000: // CPS, SETEND
		in_.kind = .Cps
	}
	in_.cond = 14
}

@(private)
decode_000 :: proc(in_: ^Inst) {
	raw := in_.raw
	op := bits(raw, 24, 21)
	s := bit(raw, 20)
	if bit(raw, 7) && bit(raw, 4) {
		decode_mul_extra(in_)
		return
	}
	if !s && op & 0xC == 0x8 { // misc: 10xx with S=0
		decode_misc(in_)
		return
	}
	in_.kind = .DataProc
	in_.op = int(op)
	in_.s = s
	in_.rn = int(bits(raw, 19, 16))
	in_.rd = int(bits(raw, 15, 12))
	if bit(raw, 4) {
		in_.op2 = Operand{rm = int(bits(raw, 3, 0)), shift = int(bits(raw, 6, 5)), reg_shift = true, rs = int(bits(raw, 11, 8))}
	} else {
		in_.op2 = shifter_imm(raw)
	}
}

@(private)
decode_001 :: proc(in_: ^Inst) {
	raw := in_.raw
	op := bits(raw, 24, 21)
	s := bit(raw, 20)
	if !s && op & 0xC == 0x8 {
		if op & 1 == 1 { // MSR imm (op 1001 / 1011), or hints
			if bits(raw, 19, 16) == 0 && op == 9 {
				in_.kind = .Hint
				return
			}
			in_.kind = .Msr
			in_.mask = bits(raw, 19, 16)
			in_.op = int(bits(raw, 22, 22)) // 1 = SPSR
			rot := bits(raw, 11, 8) * 2
			in_.op2 = Operand{imm = true, value = ror(raw & 0xFF, rot), rot = rot}
			return
		}
		return // MOVW/MOVT (v6T2): unknown here
	}
	in_.kind = .DataProc
	in_.op = int(op)
	in_.s = s
	in_.rn = int(bits(raw, 19, 16))
	in_.rd = int(bits(raw, 15, 12))
	rot := bits(raw, 11, 8) * 2
	in_.op2 = Operand{imm = true, value = ror(raw & 0xFF, rot), rot = rot}
}

@(private)
decode_misc :: proc(in_: ^Inst) {
	raw := in_.raw
	op2 := bits(raw, 7, 4)
	op := bits(raw, 22, 21)
	switch {
	case op2 == 0 && op & 1 == 0: // MRS
		in_.kind = .Mrs
		in_.rd = int(bits(raw, 15, 12))
		in_.op = int(op >> 1)
	case op2 == 0 && op & 1 == 1: // MSR reg
		in_.kind = .Msr
		in_.mask = bits(raw, 19, 16)
		in_.op = int(op >> 1)
		in_.op2 = Operand{rm = int(bits(raw, 3, 0))}
	case op2 == 1 && op == 1:
		in_.kind = .BranchReg
		in_.rm = int(bits(raw, 3, 0))
	case op2 == 1 && op == 3:
		in_.kind = .Clz
		in_.rd = int(bits(raw, 15, 12))
		in_.rm = int(bits(raw, 3, 0))
	case op2 == 3 && op == 1:
		in_.kind = .BranchReg
		in_.link = true
		in_.rm = int(bits(raw, 3, 0))
	case op2 == 5:
		in_.kind = .Sat
		in_.op = int(op) // 0 QADD, 1 QSUB, 2 QDADD, 3 QDSUB
		in_.rd = int(bits(raw, 15, 12))
		in_.rn = int(bits(raw, 19, 16))
		in_.rm = int(bits(raw, 3, 0))
	case op2 == 7 && op == 1:
		in_.kind = .Bkpt
		in_.imm = bits(raw, 19, 8) << 4 | bits(raw, 3, 0)
	case op2 & 9 == 8: // signed multiplies, halfwords
		in_.kind = .MulHalf
		in_.op = int(op) // 0 SMLAxy, 1 SMLAWy/SMULWy, 2 SMLALxy, 3 SMULxy
		in_.rd = int(bits(raw, 19, 16))
		in_.rn = int(bits(raw, 15, 12))
		in_.rs = int(bits(raw, 11, 8))
		in_.rm = int(bits(raw, 3, 0))
		in_.x = bit(raw, 5)
		in_.y = bit(raw, 6)
		if op == 1 && in_.x { // SMULWy (no accumulate)
			in_.op = 4
		}
	}
}

@(private)
decode_mul_extra :: proc(in_: ^Inst) {
	raw := in_.raw
	sh := bits(raw, 6, 5)
	if sh == 0 {
		if bit(raw, 24) { // swap / exclusives
			op := bits(raw, 23, 20)
			in_.rn = int(bits(raw, 19, 16))
			in_.rd = int(bits(raw, 15, 12))
			in_.rm = int(bits(raw, 3, 0))
			switch {
			case op == 0 || op == 4:
				in_.kind = .Swp
				in_.size = 1 if op == 4 else 4
			case op & 8 != 0:
				in_.kind = .MemEx
				in_.load = op & 1 != 0
				sizes := [4]int{4, 8, 1, 2}
				in_.size = sizes[(op >> 1) & 3]
			}
			return
		}
		op := bits(raw, 23, 21)
		in_.s = bit(raw, 20)
		in_.rd = int(bits(raw, 19, 16))
		in_.rn = int(bits(raw, 15, 12))
		in_.rs = int(bits(raw, 11, 8))
		in_.rm = int(bits(raw, 3, 0))
		switch op {
		case 0, 1:
			in_.kind = .Mul
			in_.op = int(op) // 0 MUL, 1 MLA
		case 2:
			in_.kind = .MulLong
			in_.op = 4 // UMAAL
		case 3:
			in_.kind = .Mul
			in_.op = 2 // MLS (v6T2)
		case 4, 5, 6, 7:
			in_.kind = .MulLong
			in_.op = int(op - 4) // 0 UMULL, 1 UMLAL, 2 SMULL, 3 SMLAL
		}
		return
	}
	// LDRH/STRH/LDRSB/LDRSH/LDRD/STRD
	in_.kind = .Mem
	in_.p, in_.u, in_.w = bit(raw, 24), bit(raw, 23), bit(raw, 21)
	l := bit(raw, 20)
	in_.rn = int(bits(raw, 19, 16))
	in_.rd = int(bits(raw, 15, 12))
	if bit(raw, 22) {
		in_.off = Operand{imm = true, value = bits(raw, 11, 8) << 4 | bits(raw, 3, 0)}
	} else {
		in_.off = Operand{rm = int(bits(raw, 3, 0))}
	}
	switch {
	case sh == 1:
		in_.load, in_.size = l, 2
	case sh == 2 && l:
		in_.load, in_.size, in_.signed = true, 1, true
	case sh == 3 && l:
		in_.load, in_.size, in_.signed = true, 2, true
	case sh == 2:
		in_.load, in_.size = true, 8 // LDRD
	case sh == 3:
		in_.load, in_.size = false, 8 // STRD
	}
	if !in_.p && in_.w {
		in_.kind = .Unknown // LDRHT etc.
	}
}

@(private)
decode_mem :: proc(in_: ^Inst, reg: bool) {
	raw := in_.raw
	in_.kind = .Mem
	in_.p, in_.u, in_.w, in_.load = bit(raw, 24), bit(raw, 23), bit(raw, 21), bit(raw, 20)
	in_.size = 4
	if bit(raw, 22) {
		in_.size = 1
	}
	in_.rn = int(bits(raw, 19, 16))
	in_.rd = int(bits(raw, 15, 12))
	if reg {
		in_.off = shifter_imm(raw)
	} else {
		in_.off = Operand{imm = true, value = raw & 0xFFF}
	}
	if !in_.p && in_.w {
		in_.kind = .Unknown // LDRT/STRT
	}
}

@(private)
decode_media :: proc(in_: ^Inst) {
	raw := in_.raw
	op1 := bits(raw, 24, 20)
	op2 := bits(raw, 7, 5)
	rn := int(bits(raw, 19, 16))
	in_.rd = int(bits(raw, 15, 12))
	in_.rm = int(bits(raw, 3, 0))
	switch {
	case op1 >> 3 == 0: // parallel add/sub: 00 pp op
		in_.kind = .Parallel
		in_.op = int(bits(raw, 21, 20)) << 3 | int(op2)
		in_.op |= int((raw >> 22) & 1) << 5
		in_.rn = rn
		in_.rd = int(bits(raw, 15, 12))
		if bits(raw, 21, 20) == 0 || op2 == 5 || op2 == 6 {
			in_.kind = .Unknown
		}
	case op1 == 8 && op2 & 1 == 0: // PKH
		in_.kind = .Pkh
		in_.op = int(bits(raw, 6, 6))
		in_.rn = rn
		in_.shift = bits(raw, 11, 7)
		if in_.op == 1 && in_.shift == 0 {
			in_.shift = 32
		}
	case op1 >> 1 == 5 && op2 & 1 == 0, op1 >> 1 == 7 && op2 & 1 == 0: // SSAT / USAT
		in_.kind = .Ssat
		in_.op = int(bits(raw, 22, 22))
		in_.imm = bits(raw, 20, 16)
		in_.shift_t = int(bits(raw, 6, 6)) * 2 // LSL or ASR
		in_.shift = bits(raw, 11, 7)
		if in_.shift_t == ASR && in_.shift == 0 {
			in_.shift = 32
		}
	case (op1 == 10 || op1 == 14) && op2 == 1: // SSAT16 / USAT16
		in_.kind = .Sat16
		in_.op = int(bits(raw, 22, 22))
		in_.imm = bits(raw, 19, 16)
	case op2 == 3 && op1 >> 3 == 1 && bits(raw, 9, 8) == 0: // extend
		in_.kind = .Extend
		in_.op = int(bits(raw, 22, 20)) // 0 SXTB16, 2 SXTB, 3 SXTH, 4 UXTB16, 6 UXTB, 7 UXTH
		in_.rn = rn                     // 15: no add
		in_.rot = bits(raw, 11, 10) * 8
		if in_.op == 1 || in_.op == 5 {
			in_.kind = .Unknown
		}
	case op1 == 8 && op2 == 5: // SEL
		in_.kind = .Sel
		in_.rn = rn
	case op1 == 11 && op2 == 1:
		in_.kind, in_.op = .Rev, 0
	case op1 == 11 && op2 == 5:
		in_.kind, in_.op = .Rev, 1
	case op1 == 15 && op2 == 5:
		in_.kind, in_.op = .Rev, 2
	case op1 >> 3 == 2: // signed multiplies (SMLAD etc.)
		in_.kind = .MulDual
		in_.rd = rn
		in_.rn = int(bits(raw, 15, 12))
		in_.rs = int(bits(raw, 11, 8))
		in_.x = bit(raw, 5)
		switch {
		case op1 == 16 && op2 >> 1 == 0:
			in_.op = 0 // SMLAD (Rn=15: SMUAD)
		case op1 == 16 && op2 >> 1 == 1:
			in_.op = 1 // SMLSD (SMUSD)
		case op1 == 20 && op2 >> 1 == 0:
			in_.op = 2 // SMLALD
		case op1 == 20 && op2 >> 1 == 1:
			in_.op = 3 // SMLSLD
		case op1 == 21 && op2 >> 1 == 0:
			in_.op = 4 // SMMLA (SMMUL)
		case op1 == 21 && op2 >> 1 == 3:
			in_.op = 5 // SMMLS
		case:
			in_.kind = .Unknown
		}
	case op1 == 24 && op2 == 0:
		in_.kind = .MulDual
		in_.op = 6 // USADA8 (Rn=15: USAD8)
		in_.rd = rn
		in_.rn = int(bits(raw, 15, 12))
		in_.rs = int(bits(raw, 11, 8))
	}
}

@(private)
decode_coproc_mem :: proc(in_: ^Inst) {
	raw := in_.raw
	cp := bits(raw, 11, 8)
	if bits(raw, 24, 21) == 2 && (cp == 10 || cp == 11) { // MCRR/MRRC 1100 010x
		in_.kind = .VfpMov2
		in_.load = bit(raw, 20)
		in_.dbl = cp == 11
		in_.rd = int(bits(raw, 15, 12))
		in_.rn = int(bits(raw, 19, 16))
		if in_.dbl {
			in_.vm = int(bits(raw, 3, 0)) | int(bits(raw, 5, 5)) << 4
		} else {
			in_.vm = int(bits(raw, 3, 0)) << 1 | int(bits(raw, 5, 5))
		}
		return
	}
	if cp != 10 && cp != 11 {
		return
	}
	p, u, w, l := bit(raw, 24), bit(raw, 23), bit(raw, 21), bit(raw, 20)
	in_.dbl = cp == 11
	in_.load = l
	in_.rn = int(bits(raw, 19, 16))
	if in_.dbl {
		in_.vd = int(bits(raw, 15, 12)) | int(bits(raw, 22, 22)) << 4
	} else {
		in_.vd = int(bits(raw, 15, 12)) << 1 | int(bits(raw, 22, 22))
	}
	imm := raw & 0xFF
	if p && !w {
		in_.kind = .VfpMem
		in_.u = u
		in_.imm = imm * 4
		return
	}
	in_.kind = .VfpBlock
	in_.p, in_.u, in_.w = p, u, w
	if in_.dbl {
		if imm & 1 != 0 { // FLDMX/FSTMX
			in_.op = 1
		}
		in_.count = int(imm / 2)
	} else {
		in_.count = int(imm)
	}
}

@(private)
vfp_reg :: proc(dbl: bool, v, extra: u32) -> int {
	if dbl {
		return int(v | extra << 4)
	}
	return int(v << 1 | extra)
}

@(private)
decode_coproc :: proc(in_: ^Inst) {
	raw := in_.raw
	cp := int(bits(raw, 11, 8))
	if cp != 10 && cp != 11 {
		if bit(raw, 4) {
			in_.kind = .CoprocReg
			in_.load = bit(raw, 20)
			in_.cp = cp
			in_.opc1 = int(bits(raw, 23, 21))
			in_.rd = int(bits(raw, 15, 12))
			in_.crn = int(bits(raw, 19, 16))
			in_.crm = int(bits(raw, 3, 0))
			in_.opc2 = int(bits(raw, 7, 5))
		}
		return
	}
	dbl := cp == 11
	if bit(raw, 4) { // register transfers
		opc1 := bits(raw, 23, 21)
		in_.load = bit(raw, 20)
		in_.rd = int(bits(raw, 15, 12))
		switch {
		case !dbl && opc1 == 0:
			in_.kind = .VfpMovCore
			in_.vn = int(bits(raw, 19, 16)) << 1 | int(bits(raw, 7, 7))
		case !dbl && opc1 == 7:
			in_.kind = .VfpSys
			in_.reg = int(bits(raw, 19, 16))
		case dbl && opc1 <= 1:
			in_.kind = .VfpMovHalf
			in_.vn = int(bits(raw, 19, 16)) | int(bits(raw, 7, 7)) << 4
			in_.x = opc1 == 1
		}
		return
	}
	in_.dbl = dbl
	in_.vd = vfp_reg(dbl, bits(raw, 15, 12), bits(raw, 22, 22))
	in_.vn = vfp_reg(dbl, bits(raw, 19, 16), bits(raw, 7, 7))
	in_.vm = vfp_reg(dbl, bits(raw, 3, 0), bits(raw, 5, 5))
	opc1 := bits(raw, 23, 20) &~ 4 // drop D
	p := bit(raw, 6)
	switch opc1 {
	case 0:
		in_.kind, in_.op = .VfpArith, VMLS if p else VMLA
	case 1:
		in_.kind, in_.op = .VfpArith, VNMLA if p else VNMLS
	case 2:
		in_.kind, in_.op = .VfpArith, VNMUL if p else VMUL
	case 3:
		in_.kind, in_.op = .VfpArith, VSUB if p else VADD
	case 8:
		if !p {
			in_.kind, in_.op = .VfpArith, VDIV
		}
	case 0xB:
		decode_vfp_other(in_, dbl)
	}
}

@(private)
decode_vfp_other :: proc(in_: ^Inst, dbl: bool) {
	raw := in_.raw
	opc2 := bits(raw, 19, 16)
	opc3 := bits(raw, 7, 6)
	if opc3 & 1 == 0 { // VMOV imm (VFPv3)
		return
	}
	m := bits(raw, 3, 0)
	mbit := bits(raw, 5, 5)
	vm_s := int(m << 1 | mbit)
	vm_d := int(m | mbit << 4)
	switch opc2 {
	case 0:
		in_.kind = .VfpArith
		in_.op = VABS if opc3 == 3 else VMOV
	case 1:
		in_.kind = .VfpArith
		in_.op = VSQRT if opc3 == 3 else VNEG
	case 4, 5:
		in_.kind = .VfpCmp
		in_.e = opc3 == 3
		in_.zero = opc2 == 5
	case 7: // VCVT between double and single
		if opc3 == 3 {
			in_.kind = .VfpCvt
			if dbl { // Sd = (float)Dm
				in_.op = CVT_F32_F64
				in_.vd = int(bits(raw, 15, 12) << 1 | bits(raw, 22, 22))
				in_.vm = vm_d
			} else {
				in_.op = CVT_F64_F32
				in_.vd = int(bits(raw, 15, 12) | bits(raw, 22, 22) << 4)
				in_.vm = vm_s
			}
		}
	case 8: // int -> float
		in_.kind = .VfpCvt
		signed := bit(raw, 7)
		in_.vm = vm_s
		switch {
		case dbl && signed:
			in_.op = CVT_F64_S32
		case dbl:
			in_.op = CVT_F64_U32
		case signed:
			in_.op = CVT_F32_S32
		case:
			in_.op = CVT_F32_U32
		}
	case 12, 13: // float -> int
		in_.kind = .VfpCvt
		in_.vd = int(bits(raw, 15, 12) << 1 | bits(raw, 22, 22))
		in_.mask = 0
		if opc3 == 1 { // R bit clear: use FPSCR rounding
			in_.e = true
		}
		signed := opc2 == 13
		switch {
		case dbl && signed:
			in_.op = CVT_S32_F64
		case dbl:
			in_.op = CVT_U32_F64
		case signed:
			in_.op = CVT_S32_F32
		case:
			in_.op = CVT_U32_F32
		}
	}
}

// is_return reports whether the instruction returns to the caller in the
// AAPCS sense (bx lr, mov pc lr, pop {..pc}, ldr pc [sp], #4).
is_return :: proc(in_: Inst) -> bool {
	#partial switch in_.kind {
	case .BranchReg:
		return !in_.link && in_.rm == 14
	case .DataProc:
		return in_.rd == 15 && in_.op == MOV && !in_.op2.imm && !in_.op2.reg_shift && in_.op2.rm == 14 && in_.op2.shift == LSL && in_.op2.amount == 0
	case .Block:
		return in_.load && in_.rn == 13 && in_.reg_list & 0x8000 != 0
	case .Mem:
		return in_.load && in_.rd == 15 && in_.rn == 13 && !in_.p && in_.u && in_.size == 4 && in_.off.imm
	}
	return false
}

// writes_pc reports whether the instruction can write the PC.
writes_pc :: proc(in_: Inst) -> bool {
	#partial switch in_.kind {
	case .Branch, .BlxImm, .BranchReg:
		return true
	case .DataProc:
		return in_.rd == 15 && (in_.op < TST || in_.op > CMN)
	case .Block:
		return in_.load && in_.reg_list & 0x8000 != 0
	case .Mem:
		return in_.load && (in_.rd == 15 || (in_.size == 8 && in_.rd == 14))
	}
	return false
}
