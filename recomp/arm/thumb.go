package arm

// DecodeThumb decodes the Thumb (ARMv6, 16-bit plus the BL/BLX pair)
// instruction at addr into the same form as ARM instructions: hw is the
// halfword at addr, next the one after (used by BL/BLX).
func DecodeThumb(addr uint32, hw, next uint16) Inst {
	raw := uint32(hw)
	in := Inst{Addr: addr, Raw: raw, Cond: 14, Thumb: true, Len: 2, Rn: -1, Rd: -1, Rm: -1, Rs: -1, Rd2: -1}
	r := func(hi, lo uint) int { return int(bits(raw, hi, lo)) }
	dp := func(op int, s bool, rd, rn int, o Operand) {
		in.Kind, in.Op, in.S, in.Rd, in.Rn, in.Op2 = DataProc, op, s, rd, rn, o
	}
	reg := func(rm int) Operand { return Operand{Rm: rm, Shift: LSL} }
	imm := func(v uint32) Operand { return Operand{Imm: true, Value: v} }
	mem := func(load bool, size int, signed bool, rd, rn int, off Operand) {
		in.Kind, in.Load, in.Size, in.Signed, in.Rd, in.Rn, in.Off = Mem, load, size, signed, rd, rn, off
		in.P, in.U, in.W = true, true, false
	}
	switch {
	case raw>>11 < 3: // LSL/LSR/ASR imm
		t := bits(raw, 12, 11)
		amt := bits(raw, 10, 6)
		dp(MOV, true, r(2, 0), 0, Operand{Rm: r(5, 3), Shift: shiftKind(t, amt), Amount: shiftAmt(t, amt)})
		if t == LSL && amt == 0 {
			in.Op2.Shift = LSL
		}
	case raw>>11 == 3: // ADD/SUB reg/imm3
		op := ADD
		if bit(raw, 9) {
			op = SUB
		}
		o := reg(r(8, 6))
		if bit(raw, 10) {
			o = imm(bits(raw, 8, 6))
		}
		dp(op, true, r(2, 0), r(5, 3), o)
	case raw>>13 == 1: // MOV/CMP/ADD/SUB imm8
		rd := r(10, 8)
		v := imm(raw & 0xFF)
		switch bits(raw, 12, 11) {
		case 0:
			dp(MOV, true, rd, 0, v)
		case 1:
			dp(CMP, true, 0, rd, v)
		case 2:
			dp(ADD, true, rd, rd, v)
		case 3:
			dp(SUB, true, rd, rd, v)
		}
	case raw>>10 == 0x10: // ALU
		rd, rm := r(2, 0), r(5, 3)
		switch bits(raw, 9, 6) {
		case 0:
			dp(AND, true, rd, rd, reg(rm))
		case 1:
			dp(EOR, true, rd, rd, reg(rm))
		case 2, 3, 4, 7: // LSL/LSR/ASR/ROR by register
			t := map[uint32]int{2: LSL, 3: LSR, 4: ASR, 7: ROR}[bits(raw, 9, 6)]
			dp(MOV, true, rd, 0, Operand{Rm: rd, Shift: t, RegShift: true, Rs: rm})
		case 5:
			dp(ADC, true, rd, rd, reg(rm))
		case 6:
			dp(SBC, true, rd, rd, reg(rm))
		case 8:
			dp(TST, true, 0, rd, reg(rm))
		case 9: // NEG
			dp(RSB, true, rd, rm, imm(0))
		case 10:
			dp(CMP, true, 0, rd, reg(rm))
		case 11:
			dp(CMN, true, 0, rd, reg(rm))
		case 12:
			dp(ORR, true, rd, rd, reg(rm))
		case 13:
			in.Kind, in.Op, in.S, in.Rd, in.Rm, in.Rs = Mul, 0, true, rd, rm, rd
		case 14:
			dp(BIC, true, rd, rd, reg(rm))
		case 15:
			dp(MVN, true, rd, 0, reg(rm))
		}
	case raw>>10 == 0x11: // hi-register ops, BX/BLX
		rd := r(2, 0) | r(7, 7)<<3
		rm := r(6, 3)
		switch bits(raw, 9, 8) {
		case 0:
			dp(ADD, false, rd, rd, reg(rm))
		case 1:
			dp(CMP, true, 0, rd, reg(rm))
		case 2:
			dp(MOV, false, rd, 0, reg(rm))
		case 3:
			in.Kind, in.Rm, in.Link = BranchReg, rm, bit(raw, 7)
		}
	case raw>>11 == 9: // LDR literal
		mem(true, 4, false, r(10, 8), 15, imm((raw&0xFF)*4))
		in.AlignPC = true
	case raw>>12 == 5: // load/store register offset
		rd, rn, rm := r(2, 0), r(5, 3), r(8, 6)
		o := reg(rm)
		switch bits(raw, 11, 9) {
		case 0:
			mem(false, 4, false, rd, rn, o)
		case 1:
			mem(false, 2, false, rd, rn, o)
		case 2:
			mem(false, 1, false, rd, rn, o)
		case 3:
			mem(true, 1, true, rd, rn, o)
		case 4:
			mem(true, 4, false, rd, rn, o)
		case 5:
			mem(true, 2, false, rd, rn, o)
		case 6:
			mem(true, 1, false, rd, rn, o)
		case 7:
			mem(true, 2, true, rd, rn, o)
		}
	case raw>>13 == 3: // STR/LDR/STRB/LDRB imm5
		b := bit(raw, 12)
		off := bits(raw, 10, 6)
		size := 4
		if b {
			size = 1
		} else {
			off *= 4
		}
		mem(bit(raw, 11), size, false, r(2, 0), r(5, 3), imm(off))
	case raw>>12 == 8: // STRH/LDRH imm5
		mem(bit(raw, 11), 2, false, r(2, 0), r(5, 3), imm(bits(raw, 10, 6)*2))
	case raw>>12 == 9: // SP-relative
		mem(bit(raw, 11), 4, false, r(10, 8), 13, imm((raw&0xFF)*4))
	case raw>>12 == 10: // ADD rd, pc/sp, imm
		if bit(raw, 11) {
			dp(ADD, false, r(10, 8), 13, imm((raw&0xFF)*4))
		} else {
			dp(ADD, false, r(10, 8), 15, imm((raw&0xFF)*4))
			in.AlignPC = true
		}
	case raw>>12 == 11: // misc
		decodeThumbMisc(&in)
	case raw>>12 == 12: // STMIA/LDMIA
		in.Kind = Block
		in.Load = bit(raw, 11)
		in.Rn = r(10, 8)
		in.RegList = uint16(raw & 0xFF)
		in.P, in.U = false, true
		in.W = !(in.Load && in.RegList>>uint(in.Rn)&1 != 0)
	case raw>>12 == 13: // B cond, SVC
		c := bits(raw, 11, 8)
		switch c {
		case 15:
			in.Kind = Svc
			in.Imm = raw & 0xFF
		case 14: // UDF
		default:
			in.Kind = Branch
			in.Cond = int(c)
			in.Target = addr + 4 + uint32(int32(raw<<24)>>23)
		}
	case raw>>11 == 0x1C: // B
		in.Kind = Branch
		in.Target = addr + 4 + uint32(int32(raw<<21)>>20)
	case raw>>11 == 0x1E: // BL/BLX prefix
		n := uint32(next)
		hiOff := uint32(int32(raw<<21) >> 9) // imm11 << 12, signed
		switch n >> 11 {
		case 0x1F: // BL
			in.Kind, in.Link, in.Len = Branch, true, 4
			in.Target = addr + 4 + hiOff + (n&0x7FF)<<1
			in.Raw = raw | n<<16
		case 0x1D: // BLX to ARM
			in.Kind, in.Link, in.Len, in.Exchange = Branch, true, 4, true
			in.Target = (addr + 4 + hiOff + (n&0x7FF)<<1) &^ 3
			in.Raw = raw | n<<16
		}
	}
	return in
}

func decodeThumbMisc(in *Inst) {
	raw := in.Raw
	r := func(hi, lo uint) int { return int(bits(raw, hi, lo)) }
	switch {
	case raw>>8 == 0xB0: // ADD/SUB sp, imm7
		op := ADD
		if bit(raw, 7) {
			op = SUB
		}
		in.Kind, in.Op, in.Rd, in.Rn = DataProc, op, 13, 13
		in.Op2 = Operand{Imm: true, Value: (raw & 0x7F) * 4}
	case raw>>8 == 0xB2: // SXTH/SXTB/UXTH/UXTB
		in.Kind = Extend
		in.Op = [4]int{3, 2, 7, 6}[bits(raw, 7, 6)]
		in.Rd, in.Rm, in.Rn = r(2, 0), r(5, 3), 15
	case raw>>9 == 0x5A: // PUSH
		in.Kind = Block
		in.Rn = 13
		in.RegList = uint16(raw & 0xFF)
		if bit(raw, 8) {
			in.RegList |= 1 << 14
		}
		in.P, in.U, in.W = true, false, true
	case raw>>9 == 0x5E: // POP
		in.Kind = Block
		in.Load = true
		in.Rn = 13
		in.RegList = uint16(raw & 0xFF)
		if bit(raw, 8) {
			in.RegList |= 1 << 15
		}
		in.P, in.U, in.W = false, true, true
	case raw>>8 == 0xBA: // REV / REV16 / REVSH
		op := bits(raw, 7, 6)
		if op != 2 {
			in.Kind = Rev
			in.Op = [4]int{0, 1, 0, 2}[op]
			in.Rd, in.Rm = r(2, 0), r(5, 3)
		}
	case raw>>8 == 0xBE:
		in.Kind = Bkpt
		in.Imm = raw & 0xFF
	case raw>>5 == 0x5B3 || raw == 0xB650 || raw == 0xB658: // CPS, SETEND
		in.Kind = Cps
	case raw>>8 == 0xBF && raw&0xF == 0: // NOP-compatible hints
		in.Kind = Hint
	}
}
