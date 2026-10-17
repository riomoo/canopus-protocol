// Package arm decodes the ARM (A32) instructions of an ARMv6K + VFPv2
// program, as armcc emits them for the 3DS application core. Thumb is not
// decoded: the game has no Thumb code.
package arm

import "fmt"

// Kind is the instruction class the code generator switches on.
type Kind int

const (
	Unknown   Kind = iota
	DataProc       // AND..MVN: Op, S, Rd, Rn, Op2
	Mul            // MUL/MLA/MLS (Op: MUL, MLA): Rd, Rn(=acc), Rm, Rs, S
	MulLong        // UMULL/UMLAL/SMULL/SMLAL/UMAAL: RdLo(Rd), RdHi(Rn), Rm, Rs, S, Op
	MulHalf        // SMLAxy/SMULxy/SMULWy/SMLAWy/SMLALxy: Op, Rd, Rn(acc), Rm, Rs, X, Y
	MulDual        // SMUAD/SMLAD/SMUSD/SMLSD/SMLALD/SMLSLD/SMMUL/SMMLA/SMMLS/USAD8/USADA8: Op, Rd, Rn(acc), Rm, Rs, X
	Clz            // Rd, Rm
	Sat            // QADD/QSUB/QDADD/QDSUB: Op, Rd, Rm, Rn
	Mrs            // Rd
	Msr            // Mask, Op2 (imm or reg)
	Hint           // NOP/YIELD/WFE/WFI/SEV and other no-op hints
	Branch         // B/BL: Link, Target
	BlxImm         // BLX imm (to Thumb): Target
	BranchReg      // BX/BLX reg: Link, Rm
	Mem            // LDR/STR word/byte/half/signed/double: Load, Size, Signed, Rd, Rn, Off, P, U, W
	MemEx          // LDREX/STREX (B/H/D): Load, Size, Rd, Rn, Rm (STREX value), Rd2 for D
	Swp            // SWP/SWPB: Size, Rd, Rm, Rn
	Clrex
	Block      // LDM/STM: Load, Rn, RegList, P, U, W
	Svc        // Imm
	Bkpt       // Imm
	Extend     // SXTB/SXTH/UXTB/UXTH/SXTAB/... /SXTB16/UXTB16: Op, Rd, Rn (15 = none), Rm, Rot
	Pkh        // PKHBT/PKHTB: Op(0 BT, 1 TB), Rd, Rn, Rm, Shift
	Ssat       // SSAT/USAT: Op(0 S, 1 U), Rd, SatImm, Rm, ShiftType, Shift
	Sat16      // SSAT16/USAT16: Op, Rd, SatImm, Rm
	Rev        // REV/REV16/REVSH: Op, Rd, Rm
	Sel        // Rd, Rn, Rm
	Parallel   // SADD16..UHSUB8: Op (prefix<<3 | op), Rd, Rn, Rm
	Pld        // preload (no-op)
	Cps        // CPS/SETEND (ignored in user mode)
	CoprocReg  // MRC/MCR (non-VFP): Load(=MRC), Cp, Opc1, Rd, CRn, CRm, Opc2
	VfpArith   // VFP data processing: Op, Dbl, Vd, Vn, Vm
	VfpCmp     // VCMP/VCMPE (Zero: compare with 0.0): Dbl, Vd, Vm, E
	VfpCvt     // conversions: Op, Vd, Vm
	VfpMovCore // VMOV Sn <-> Rt: Load(=to core), Rd, Vn
	VfpMovHalf // VMOV Dn[x] <-> Rt (FMDLR/FMDHR/FMRDL/FMRDH): Load, Rd, Vn, X
	VfpMov2    // VMOV Rt,Rt2 <-> Sm,Sm+1 or Dm: Load, Dbl, Rd, Rn(=Rt2), Vm
	VfpSys     // VMRS/VMSR: Load(=VMRS), Rd (15 = APSR_nzcv), Reg
	VfpMem     // VLDR/VSTR: Load, Dbl, Vd, Rn, Off(imm), U
	VfpBlock   // VLDM/VSTM: Load, Dbl, Vd, Count, Rn, P, U, W
)

// Shift types.
const (
	LSL = 0
	LSR = 1
	ASR = 2
	ROR = 3
	RRX = 4
)

// Operand is a flexible second operand or a memory offset.
type Operand struct {
	Imm      bool   // an immediate (Value)
	Value    uint32 // immediate value
	Rot      uint32 // data-processing immediate rotation (carry out when non-zero)
	Rm       int
	Shift    int    // LSL/LSR/ASR/ROR/RRX
	Amount   uint32 // immediate shift amount (already normalised: LSR/ASR #0 -> 32)
	RegShift bool   // shift by register Rs
	Rs       int
}

// Data-processing opcodes.
const (
	AND = iota
	EOR
	SUB
	RSB
	ADD
	ADC
	SBC
	RSC
	TST
	TEQ
	CMP
	CMN
	ORR
	MOV
	BIC
	MVN
)

var DPNames = [16]string{"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"}

// VFP arithmetic ops.
const (
	VMLA = iota
	VMLS
	VNMLS
	VNMLA
	VMUL
	VNMUL
	VADD
	VSUB
	VDIV
	VMOV // register move
	VABS
	VNEG
	VSQRT
)

// VFP conversions.
const (
	CvtF64F32 = iota // Dd = (double)Sm
	CvtF32F64        // Sd = (float)Dm
	CvtF32U32        // Sd = (float)(u32)Sm
	CvtF32S32        // Sd = (float)(s32)Sm
	CvtF64U32        // Dd = (double)(u32)Sm
	CvtF64S32        // Dd = (double)(s32)Sm
	CvtU32F32        // Sd = (u32)Sm, round toward zero (R: FPSCR rounding)
	CvtS32F32
	CvtU32F64
	CvtS32F64
)

// Inst is one decoded instruction.
type Inst struct {
	Addr   uint32
	Raw    uint32
	Cond   int // 0..14, 14 = always
	Kind   Kind
	Op     int
	S      bool
	Rd, Rn int
	Rm, Rs int
	Rd2    int
	Op2    Operand
	// memory
	Load, Signed bool
	Size         int // 1, 2, 4, 8
	P, U, W      bool
	RegList      uint16
	Off          Operand
	// misc
	Link   bool
	Target uint32
	Imm    uint32
	Mask   uint32
	X, Y   bool
	E      bool
	Zero   bool
	Dbl    bool
	Vd, Vn int
	Vm     int
	Count  int
	Cp     int
	Opc1   int
	Opc2   int
	CRn    int
	CRm    int
	Rot    uint32
	ShiftT int
	Shift  uint32
	Reg    int
	// Thumb
	Thumb    bool
	Len      uint32 // 4 (ARM, Thumb BL pair) or 2
	AlignPC  bool   // Thumb LDR literal / ADR: PC reads as Align(addr+4, 4)
	Exchange bool   // BLX imm: the target is in the other instruction set
}

// PC is the value a read of r15 gives in this instruction.
func (in *Inst) PC() uint32 {
	if !in.Thumb {
		return in.Addr + 8
	}
	if in.AlignPC {
		return (in.Addr + 4) &^ 3
	}
	return in.Addr + 4
}

func bits(x uint32, hi, lo uint) uint32 { return (x >> lo) & ((1 << (hi - lo + 1)) - 1) }
func bit(x uint32, n uint) bool         { return x>>n&1 != 0 }

func ror(v, n uint32) uint32 { n &= 31; return v>>n | v<<((32-n)&31) }

func shifterImm(raw uint32) Operand {
	return Operand{Rm: int(bits(raw, 3, 0)), Shift: shiftKind(bits(raw, 6, 5), bits(raw, 11, 7)), Amount: shiftAmt(bits(raw, 6, 5), bits(raw, 11, 7))}
}

func shiftKind(t, amt uint32) int {
	if t == ROR && amt == 0 {
		return RRX
	}
	return int(t)
}

func shiftAmt(t, amt uint32) uint32 {
	if (t == LSR || t == ASR) && amt == 0 {
		return 32
	}
	return amt
}

// Decode decodes one ARM instruction at addr.
func Decode(addr, raw uint32) Inst {
	in := Inst{Addr: addr, Raw: raw, Cond: int(raw >> 28), Rn: -1, Rd: -1, Rm: -1, Rs: -1, Rd2: -1, Len: 4}
	if in.Cond == 15 {
		decodeUncond(&in)
		return in
	}
	switch bits(raw, 27, 25) {
	case 0:
		decode000(&in)
	case 1:
		decode001(&in)
	case 2:
		decodeMem(&in, false)
	case 3:
		if bit(raw, 4) {
			decodeMedia(&in)
		} else {
			decodeMem(&in, true)
		}
	case 4:
		in.Kind = Block
		in.P, in.U, in.W, in.Load = bit(raw, 24), bit(raw, 23), bit(raw, 21), bit(raw, 20)
		in.Rn = int(bits(raw, 19, 16))
		in.RegList = uint16(raw)
		if bit(raw, 22) { // user-bank / exception return forms
			in.Kind = Unknown
		}
	case 5:
		in.Kind = Branch
		in.Link = bit(raw, 24)
		off := int32(raw<<8) >> 6
		in.Target = addr + 8 + uint32(off)
	case 6:
		decodeCoprocMem(&in)
	case 7:
		if bit(raw, 24) {
			in.Kind = Svc
			in.Imm = raw & 0xFFFFFF
		} else {
			decodeCoproc(&in)
		}
	}
	return in
}

func decodeUncond(in *Inst) {
	raw := in.Raw
	switch {
	case bits(raw, 27, 25) == 5: // BLX imm: a call into Thumb
		in.Kind = Branch
		in.Link = true
		in.Exchange = true
		off := int32(raw<<8) >> 6
		in.Target = in.Addr + 8 + uint32(off) + uint32(bits(raw, 24, 24)<<1)
	case raw&0x0D70F000 == 0x0550F000: // PLD
		in.Kind = Pld
	case raw == 0xF57FF01F:
		in.Kind = Clrex
	case raw&0x0FF10020 == 0x01000000 || raw&0x0FFFFDFF == 0x01010000: // CPS, SETEND
		in.Kind = Cps
	}
	in.Cond = 14
}

func decode000(in *Inst) {
	raw := in.Raw
	op := bits(raw, 24, 21)
	s := bit(raw, 20)
	if bit(raw, 7) && bit(raw, 4) {
		decodeMulExtra(in)
		return
	}
	if !s && op&0xC == 0x8 { // misc: 10xx with S=0
		decodeMisc(in)
		return
	}
	in.Kind = DataProc
	in.Op = int(op)
	in.S = s
	in.Rn = int(bits(raw, 19, 16))
	in.Rd = int(bits(raw, 15, 12))
	if bit(raw, 4) {
		in.Op2 = Operand{Rm: int(bits(raw, 3, 0)), Shift: int(bits(raw, 6, 5)), RegShift: true, Rs: int(bits(raw, 11, 8))}
	} else {
		in.Op2 = shifterImm(raw)
	}
}

func decode001(in *Inst) {
	raw := in.Raw
	op := bits(raw, 24, 21)
	s := bit(raw, 20)
	if !s && op&0xC == 0x8 {
		if op&1 == 1 { // MSR imm (op 1001 / 1011), or hints
			if bits(raw, 19, 16) == 0 && op == 9 {
				in.Kind = Hint
				return
			}
			in.Kind = Msr
			in.Mask = bits(raw, 19, 16)
			in.Op = int(bits(raw, 22, 22)) // 1 = SPSR
			rot := bits(raw, 11, 8) * 2
			in.Op2 = Operand{Imm: true, Value: ror(raw&0xFF, rot), Rot: rot}
			return
		}
		return // MOVW/MOVT (v6T2): unknown here
	}
	in.Kind = DataProc
	in.Op = int(op)
	in.S = s
	in.Rn = int(bits(raw, 19, 16))
	in.Rd = int(bits(raw, 15, 12))
	rot := bits(raw, 11, 8) * 2
	in.Op2 = Operand{Imm: true, Value: ror(raw&0xFF, rot), Rot: rot}
}

func decodeMisc(in *Inst) {
	raw := in.Raw
	op2 := bits(raw, 7, 4)
	op := bits(raw, 22, 21)
	switch {
	case op2 == 0 && op&1 == 0: // MRS
		in.Kind = Mrs
		in.Rd = int(bits(raw, 15, 12))
		in.Op = int(op >> 1)
	case op2 == 0 && op&1 == 1: // MSR reg
		in.Kind = Msr
		in.Mask = bits(raw, 19, 16)
		in.Op = int(op >> 1)
		in.Op2 = Operand{Rm: int(bits(raw, 3, 0))}
	case op2 == 1 && op == 1:
		in.Kind = BranchReg
		in.Rm = int(bits(raw, 3, 0))
	case op2 == 1 && op == 3:
		in.Kind = Clz
		in.Rd = int(bits(raw, 15, 12))
		in.Rm = int(bits(raw, 3, 0))
	case op2 == 3 && op == 1:
		in.Kind = BranchReg
		in.Link = true
		in.Rm = int(bits(raw, 3, 0))
	case op2 == 5:
		in.Kind = Sat
		in.Op = int(op) // 0 QADD, 1 QSUB, 2 QDADD, 3 QDSUB
		in.Rd = int(bits(raw, 15, 12))
		in.Rn = int(bits(raw, 19, 16))
		in.Rm = int(bits(raw, 3, 0))
	case op2 == 7 && op == 1:
		in.Kind = Bkpt
		in.Imm = bits(raw, 19, 8)<<4 | bits(raw, 3, 0)
	case op2&9 == 8: // signed multiplies, halfwords
		in.Kind = MulHalf
		in.Op = int(op) // 0 SMLAxy, 1 SMLAWy/SMULWy, 2 SMLALxy, 3 SMULxy
		in.Rd = int(bits(raw, 19, 16))
		in.Rn = int(bits(raw, 15, 12))
		in.Rs = int(bits(raw, 11, 8))
		in.Rm = int(bits(raw, 3, 0))
		in.X = bit(raw, 5)
		in.Y = bit(raw, 6)
		if op == 1 && in.X { // SMULWy (no accumulate)
			in.Op = 4
		}
	}
}

func decodeMulExtra(in *Inst) {
	raw := in.Raw
	sh := bits(raw, 6, 5)
	if sh == 0 {
		if bit(raw, 24) { // swap / exclusives
			op := bits(raw, 23, 20)
			in.Rn = int(bits(raw, 19, 16))
			in.Rd = int(bits(raw, 15, 12))
			in.Rm = int(bits(raw, 3, 0))
			switch {
			case op == 0 || op == 4:
				in.Kind = Swp
				in.Size = 4
				if op == 4 {
					in.Size = 1
				}
			case op&8 != 0:
				in.Kind = MemEx
				in.Load = op&1 != 0
				in.Size = [4]int{4, 8, 1, 2}[(op>>1)&3]
			}
			return
		}
		op := bits(raw, 23, 21)
		in.S = bit(raw, 20)
		in.Rd = int(bits(raw, 19, 16))
		in.Rn = int(bits(raw, 15, 12))
		in.Rs = int(bits(raw, 11, 8))
		in.Rm = int(bits(raw, 3, 0))
		switch op {
		case 0, 1:
			in.Kind = Mul
			in.Op = int(op) // 0 MUL, 1 MLA
		case 2:
			in.Kind = MulLong
			in.Op = 4 // UMAAL
		case 3:
			in.Kind = Mul
			in.Op = 2 // MLS (v6T2)
		case 4, 5, 6, 7:
			in.Kind = MulLong
			in.Op = int(op - 4) // 0 UMULL, 1 UMLAL, 2 SMULL, 3 SMLAL
		}
		return
	}
	// LDRH/STRH/LDRSB/LDRSH/LDRD/STRD
	in.Kind = Mem
	in.P, in.U, in.W = bit(raw, 24), bit(raw, 23), bit(raw, 21)
	l := bit(raw, 20)
	in.Rn = int(bits(raw, 19, 16))
	in.Rd = int(bits(raw, 15, 12))
	if bit(raw, 22) {
		in.Off = Operand{Imm: true, Value: bits(raw, 11, 8)<<4 | bits(raw, 3, 0)}
	} else {
		in.Off = Operand{Rm: int(bits(raw, 3, 0))}
	}
	switch {
	case sh == 1:
		in.Load, in.Size = l, 2
	case sh == 2 && l:
		in.Load, in.Size, in.Signed = true, 1, true
	case sh == 3 && l:
		in.Load, in.Size, in.Signed = true, 2, true
	case sh == 2:
		in.Load, in.Size = true, 8 // LDRD
	case sh == 3:
		in.Load, in.Size = false, 8 // STRD
	}
	if !in.P && in.W {
		in.Kind = Unknown // LDRHT etc.
	}
}

func decodeMem(in *Inst, reg bool) {
	raw := in.Raw
	in.Kind = Mem
	in.P, in.U, in.W, in.Load = bit(raw, 24), bit(raw, 23), bit(raw, 21), bit(raw, 20)
	in.Size = 4
	if bit(raw, 22) {
		in.Size = 1
	}
	in.Rn = int(bits(raw, 19, 16))
	in.Rd = int(bits(raw, 15, 12))
	if reg {
		in.Off = shifterImm(raw)
	} else {
		in.Off = Operand{Imm: true, Value: raw & 0xFFF}
	}
	if !in.P && in.W {
		in.Kind = Unknown // LDRT/STRT
	}
}

func decodeMedia(in *Inst) {
	raw := in.Raw
	op1 := bits(raw, 24, 20)
	op2 := bits(raw, 7, 5)
	rn := int(bits(raw, 19, 16))
	in.Rd = int(bits(raw, 15, 12))
	in.Rm = int(bits(raw, 3, 0))
	switch {
	case op1>>3 == 0: // parallel add/sub: 00 pp op
		in.Kind = Parallel
		in.Op = int(bits(raw, 21, 20))<<3 | int(op2)
		in.Op |= int(bit32(raw, 22)) << 5
		in.Rn = rn
		in.Rd = int(bits(raw, 15, 12))
		if bits(raw, 21, 20) == 0 || op2 == 5 || op2 == 6 {
			in.Kind = Unknown
		}
	case op1 == 8 && op2&1 == 0: // PKH
		in.Kind = Pkh
		in.Op = int(bits(raw, 6, 6))
		in.Rn = rn
		in.Shift = bits(raw, 11, 7)
		if in.Op == 1 && in.Shift == 0 {
			in.Shift = 32
		}
	case op1>>1 == 5 && op2&1 == 0, op1>>1 == 7 && op2&1 == 0: // SSAT / USAT
		in.Kind = Ssat
		in.Op = int(bits(raw, 22, 22))
		in.Imm = bits(raw, 20, 16)
		in.ShiftT = int(bits(raw, 6, 6)) * 2 // LSL or ASR
		in.Shift = bits(raw, 11, 7)
		if in.ShiftT == ASR && in.Shift == 0 {
			in.Shift = 32
		}
	case (op1 == 10 || op1 == 14) && op2 == 1: // SSAT16 / USAT16
		in.Kind = Sat16
		in.Op = int(bits(raw, 22, 22))
		in.Imm = bits(raw, 19, 16)
	case op2 == 3 && op1>>3 == 1 && bits(raw, 9, 8) == 0: // extend
		in.Kind = Extend
		in.Op = int(bits(raw, 22, 20)) // 0 SXTB16, 2 SXTB, 3 SXTH, 4 UXTB16, 6 UXTB, 7 UXTH
		in.Rn = rn                     // 15: no add
		in.Rot = bits(raw, 11, 10) * 8
		if in.Op == 1 || in.Op == 5 {
			in.Kind = Unknown
		}
	case op1 == 8 && op2 == 5: // SEL
		in.Kind = Sel
		in.Rn = rn
	case op1 == 11 && op2 == 1:
		in.Kind, in.Op = Rev, 0
	case op1 == 11 && op2 == 5:
		in.Kind, in.Op = Rev, 1
	case op1 == 15 && op2 == 5:
		in.Kind, in.Op = Rev, 2
	case op1>>3 == 2: // signed multiplies (SMLAD etc.)
		in.Kind = MulDual
		in.Rd = rn
		in.Rn = int(bits(raw, 15, 12))
		in.Rs = int(bits(raw, 11, 8))
		in.X = bit(raw, 5)
		switch {
		case op1 == 16 && op2>>1 == 0:
			in.Op = 0 // SMLAD (Rn=15: SMUAD)
		case op1 == 16 && op2>>1 == 1:
			in.Op = 1 // SMLSD (SMUSD)
		case op1 == 20 && op2>>1 == 0:
			in.Op = 2 // SMLALD
		case op1 == 20 && op2>>1 == 1:
			in.Op = 3 // SMLSLD
		case op1 == 21 && op2>>1 == 0:
			in.Op = 4 // SMMLA (SMMUL)
		case op1 == 21 && op2>>1 == 3:
			in.Op = 5 // SMMLS
		default:
			in.Kind = Unknown
		}
	case op1 == 24 && op2 == 0:
		in.Kind = MulDual
		in.Op = 6 // USADA8 (Rn=15: USAD8)
		in.Rd = rn
		in.Rn = int(bits(raw, 15, 12))
		in.Rs = int(bits(raw, 11, 8))
	}
}

func bit32(x uint32, n uint) uint32 { return x >> n & 1 }

func decodeCoprocMem(in *Inst) {
	raw := in.Raw
	cp := bits(raw, 11, 8)
	if bits(raw, 24, 21) == 2 && (cp == 10 || cp == 11) { // MCRR/MRRC 1100 010x
		in.Kind = VfpMov2
		in.Load = bit(raw, 20)
		in.Dbl = cp == 11
		in.Rd = int(bits(raw, 15, 12))
		in.Rn = int(bits(raw, 19, 16))
		if in.Dbl {
			in.Vm = int(bits(raw, 3, 0)) | int(bits(raw, 5, 5))<<4
		} else {
			in.Vm = int(bits(raw, 3, 0))<<1 | int(bits(raw, 5, 5))
		}
		return
	}
	if cp != 10 && cp != 11 {
		return
	}
	p, u, w, l := bit(raw, 24), bit(raw, 23), bit(raw, 21), bit(raw, 20)
	in.Dbl = cp == 11
	in.Load = l
	in.Rn = int(bits(raw, 19, 16))
	if in.Dbl {
		in.Vd = int(bits(raw, 15, 12)) | int(bits(raw, 22, 22))<<4
	} else {
		in.Vd = int(bits(raw, 15, 12))<<1 | int(bits(raw, 22, 22))
	}
	imm := raw & 0xFF
	if p && !w {
		in.Kind = VfpMem
		in.U = u
		in.Imm = imm * 4
		return
	}
	in.Kind = VfpBlock
	in.P, in.U, in.W = p, u, w
	if in.Dbl {
		if imm&1 != 0 { // FLDMX/FSTMX
			in.Op = 1
		}
		in.Count = int(imm / 2)
	} else {
		in.Count = int(imm)
	}
}

func decodeCoproc(in *Inst) {
	raw := in.Raw
	cp := int(bits(raw, 11, 8))
	if cp != 10 && cp != 11 {
		if bit(raw, 4) {
			in.Kind = CoprocReg
			in.Load = bit(raw, 20)
			in.Cp = cp
			in.Opc1 = int(bits(raw, 23, 21))
			in.Rd = int(bits(raw, 15, 12))
			in.CRn = int(bits(raw, 19, 16))
			in.CRm = int(bits(raw, 3, 0))
			in.Opc2 = int(bits(raw, 7, 5))
		}
		return
	}
	dbl := cp == 11
	if bit(raw, 4) { // register transfers
		opc1 := bits(raw, 23, 21)
		in.Load = bit(raw, 20)
		in.Rd = int(bits(raw, 15, 12))
		switch {
		case !dbl && opc1 == 0:
			in.Kind = VfpMovCore
			in.Vn = int(bits(raw, 19, 16))<<1 | int(bits(raw, 7, 7))
		case !dbl && opc1 == 7:
			in.Kind = VfpSys
			in.Reg = int(bits(raw, 19, 16))
		case dbl && opc1 <= 1:
			in.Kind = VfpMovHalf
			in.Vn = int(bits(raw, 19, 16)) | int(bits(raw, 7, 7))<<4
			in.X = opc1 == 1
		}
		return
	}
	in.Dbl = dbl
	reg := func(v, extra uint32) int {
		if dbl {
			return int(v | extra<<4)
		}
		return int(v<<1 | extra)
	}
	in.Vd = reg(bits(raw, 15, 12), bits(raw, 22, 22))
	in.Vn = reg(bits(raw, 19, 16), bits(raw, 7, 7))
	in.Vm = reg(bits(raw, 3, 0), bits(raw, 5, 5))
	opc1 := bits(raw, 23, 20) &^ 4 // drop D
	p := bit(raw, 6)
	switch opc1 {
	case 0:
		in.Kind, in.Op = VfpArith, map[bool]int{false: VMLA, true: VMLS}[p]
	case 1:
		in.Kind, in.Op = VfpArith, map[bool]int{false: VNMLS, true: VNMLA}[p]
	case 2:
		in.Kind, in.Op = VfpArith, map[bool]int{false: VMUL, true: VNMUL}[p]
	case 3:
		in.Kind, in.Op = VfpArith, map[bool]int{false: VADD, true: VSUB}[p]
	case 8:
		if !p {
			in.Kind, in.Op = VfpArith, VDIV
		}
	case 0xB:
		decodeVfpOther(in, dbl)
	}
}

func decodeVfpOther(in *Inst, dbl bool) {
	raw := in.Raw
	opc2 := bits(raw, 19, 16)
	opc3 := bits(raw, 7, 6)
	if opc3&1 == 0 { // VMOV imm (VFPv3)
		return
	}
	m := bits(raw, 3, 0)
	mbit := bits(raw, 5, 5)
	vmS := int(m<<1 | mbit)
	vmD := int(m | mbit<<4)
	switch opc2 {
	case 0:
		in.Kind = VfpArith
		in.Op = map[bool]int{false: VMOV, true: VABS}[opc3 == 3]
	case 1:
		in.Kind = VfpArith
		in.Op = map[bool]int{false: VNEG, true: VSQRT}[opc3 == 3]
	case 4, 5:
		in.Kind = VfpCmp
		in.E = opc3 == 3
		in.Zero = opc2 == 5
	case 7: // VCVT between double and single
		if opc3 == 3 {
			in.Kind = VfpCvt
			if dbl { // Sd = (float)Dm
				in.Op = CvtF32F64
				in.Vd = int(bits(raw, 15, 12)<<1 | bits(raw, 22, 22))
				in.Vm = vmD
			} else {
				in.Op = CvtF64F32
				in.Vd = int(bits(raw, 15, 12) | bits(raw, 22, 22)<<4)
				in.Vm = vmS
			}
		}
	case 8: // int -> float
		in.Kind = VfpCvt
		signed := bit(raw, 7)
		in.Vm = vmS
		switch {
		case dbl && signed:
			in.Op = CvtF64S32
		case dbl:
			in.Op = CvtF64U32
		case signed:
			in.Op = CvtF32S32
		default:
			in.Op = CvtF32U32
		}
	case 12, 13: // float -> int
		in.Kind = VfpCvt
		in.Vd = int(bits(raw, 15, 12)<<1 | bits(raw, 22, 22))
		in.Mask = 0
		if opc3 == 1 { // R bit clear: use FPSCR rounding
			in.E = true
		}
		signed := opc2 == 13
		switch {
		case dbl && signed:
			in.Op = CvtS32F64
		case dbl:
			in.Op = CvtU32F64
		case signed:
			in.Op = CvtS32F32
		default:
			in.Op = CvtU32F32
		}
	}
}

// IsReturn reports whether the instruction returns to the caller in the
// AAPCS sense (bx lr, mov pc lr, pop {..pc}, ldr pc [sp], #4).
func (in *Inst) IsReturn() bool {
	switch in.Kind {
	case BranchReg:
		return !in.Link && in.Rm == 14
	case DataProc:
		return in.Rd == 15 && in.Op == MOV && !in.Op2.Imm && !in.Op2.RegShift && in.Op2.Rm == 14 && in.Op2.Shift == LSL && in.Op2.Amount == 0
	case Block:
		return in.Load && in.Rn == 13 && in.RegList&0x8000 != 0
	case Mem:
		return in.Load && in.Rd == 15 && in.Rn == 13 && !in.P && in.U && in.Size == 4 && in.Off.Imm
	}
	return false
}

// WritesPC reports whether the instruction can write the PC.
func (in *Inst) WritesPC() bool {
	switch in.Kind {
	case Branch, BlxImm, BranchReg:
		return true
	case DataProc:
		return in.Rd == 15 && (in.Op < TST || in.Op > CMN)
	case Block:
		return in.Load && in.RegList&0x8000 != 0
	case Mem:
		return in.Load && (in.Rd == 15 || (in.Size == 8 && in.Rd == 14))
	}
	return false
}

func (in Inst) String() string {
	return fmt.Sprintf("%08X: %08X kind %d op %d", in.Addr, in.Raw, in.Kind, in.Op)
}
