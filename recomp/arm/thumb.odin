package arm

@(private)
t_dp :: proc(in_: ^Inst, op: int, s: bool, rd, rn: int, o: Operand) {
	in_.kind, in_.op, in_.s, in_.rd, in_.rn, in_.op2 = .DataProc, op, s, rd, rn, o
}

@(private)
t_reg :: proc(rm: int) -> Operand { return Operand{rm = rm, shift = LSL} }

@(private)
t_imm :: proc(v: u32) -> Operand { return Operand{imm = true, value = v} }

@(private)
t_mem :: proc(in_: ^Inst, load: bool, size: int, signed: bool, rd, rn: int, off: Operand) {
	in_.kind, in_.load, in_.size, in_.signed, in_.rd, in_.rn, in_.off = .Mem, load, size, signed, rd, rn, off
	in_.p, in_.u, in_.w = true, true, false
}

@(private)
t_r :: #force_inline proc(raw: u32, hi, lo: uint) -> int { return int(bits(raw, hi, lo)) }

// decode_thumb decodes the Thumb (ARMv6, 16-bit plus the BL/BLX pair)
// instruction at addr into the same form as ARM instructions: hw is the
// halfword at addr, next the one after (used by BL/BLX).
decode_thumb :: proc(addr: u32, hw, next: u16) -> Inst {
	raw := u32(hw)
	in_ := Inst{addr = addr, raw = raw, cond = 14, thumb = true, len = 2, rn = -1, rd = -1, rm = -1, rs = -1, rd2 = -1}
	switch {
	case raw >> 11 < 3: // LSL/LSR/ASR imm
		t := bits(raw, 12, 11)
		amt := bits(raw, 10, 6)
		t_dp(&in_, MOV, true, t_r(raw, 2, 0), 0, Operand{rm = t_r(raw, 5, 3), shift = shift_kind(t, amt), amount = shift_amt(t, amt)})
		if t == LSL && amt == 0 {
			in_.op2.shift = LSL
		}
	case raw >> 11 == 3: // ADD/SUB reg/imm3
		op := ADD
		if bit(raw, 9) {
			op = SUB
		}
		o := t_reg(t_r(raw, 8, 6))
		if bit(raw, 10) {
			o = t_imm(bits(raw, 8, 6))
		}
		t_dp(&in_, op, true, t_r(raw, 2, 0), t_r(raw, 5, 3), o)
	case raw >> 13 == 1: // MOV/CMP/ADD/SUB imm8
		rd := t_r(raw, 10, 8)
		v := t_imm(raw & 0xFF)
		switch bits(raw, 12, 11) {
		case 0:
			t_dp(&in_, MOV, true, rd, 0, v)
		case 1:
			t_dp(&in_, CMP, true, 0, rd, v)
		case 2:
			t_dp(&in_, ADD, true, rd, rd, v)
		case 3:
			t_dp(&in_, SUB, true, rd, rd, v)
		}
	case raw >> 10 == 0x10: // ALU
		rd, rm := t_r(raw, 2, 0), t_r(raw, 5, 3)
		switch bits(raw, 9, 6) {
		case 0:
			t_dp(&in_, AND, true, rd, rd, t_reg(rm))
		case 1:
			t_dp(&in_, EOR, true, rd, rd, t_reg(rm))
		case 2, 3, 4, 7: // LSL/LSR/ASR/ROR by register
			t: int
			switch bits(raw, 9, 6) {
			case 2: t = LSL
			case 3: t = LSR
			case 4: t = ASR
			case 7: t = ROR
			}
			t_dp(&in_, MOV, true, rd, 0, Operand{rm = rd, shift = t, reg_shift = true, rs = rm})
		case 5:
			t_dp(&in_, ADC, true, rd, rd, t_reg(rm))
		case 6:
			t_dp(&in_, SBC, true, rd, rd, t_reg(rm))
		case 8:
			t_dp(&in_, TST, true, 0, rd, t_reg(rm))
		case 9: // NEG
			t_dp(&in_, RSB, true, rd, rm, t_imm(0))
		case 10:
			t_dp(&in_, CMP, true, 0, rd, t_reg(rm))
		case 11:
			t_dp(&in_, CMN, true, 0, rd, t_reg(rm))
		case 12:
			t_dp(&in_, ORR, true, rd, rd, t_reg(rm))
		case 13:
			in_.kind, in_.op, in_.s, in_.rd, in_.rm, in_.rs = .Mul, 0, true, rd, rm, rd
		case 14:
			t_dp(&in_, BIC, true, rd, rd, t_reg(rm))
		case 15:
			t_dp(&in_, MVN, true, rd, 0, t_reg(rm))
		}
	case raw >> 10 == 0x11: // hi-register ops, BX/BLX
		rd := t_r(raw, 2, 0) | t_r(raw, 7, 7) << 3
		rm := t_r(raw, 6, 3)
		switch bits(raw, 9, 8) {
		case 0:
			t_dp(&in_, ADD, false, rd, rd, t_reg(rm))
		case 1:
			t_dp(&in_, CMP, true, 0, rd, t_reg(rm))
		case 2:
			t_dp(&in_, MOV, false, rd, 0, t_reg(rm))
		case 3:
			in_.kind, in_.rm, in_.link = .BranchReg, rm, bit(raw, 7)
		}
	case raw >> 11 == 9: // LDR literal
		t_mem(&in_, true, 4, false, t_r(raw, 10, 8), 15, t_imm((raw & 0xFF) * 4))
		in_.align_pc = true
	case raw >> 12 == 5: // load/store register offset
		rd, rn, rm := t_r(raw, 2, 0), t_r(raw, 5, 3), t_r(raw, 8, 6)
		o := t_reg(rm)
		switch bits(raw, 11, 9) {
		case 0:
			t_mem(&in_, false, 4, false, rd, rn, o)
		case 1:
			t_mem(&in_, false, 2, false, rd, rn, o)
		case 2:
			t_mem(&in_, false, 1, false, rd, rn, o)
		case 3:
			t_mem(&in_, true, 1, true, rd, rn, o)
		case 4:
			t_mem(&in_, true, 4, false, rd, rn, o)
		case 5:
			t_mem(&in_, true, 2, false, rd, rn, o)
		case 6:
			t_mem(&in_, true, 1, false, rd, rn, o)
		case 7:
			t_mem(&in_, true, 2, true, rd, rn, o)
		}
	case raw >> 13 == 3: // STR/LDR/STRB/LDRB imm5
		b := bit(raw, 12)
		off := bits(raw, 10, 6)
		size := 4
		if b {
			size = 1
		} else {
			off *= 4
		}
		t_mem(&in_, bit(raw, 11), size, false, t_r(raw, 2, 0), t_r(raw, 5, 3), t_imm(off))
	case raw >> 12 == 8: // STRH/LDRH imm5
		t_mem(&in_, bit(raw, 11), 2, false, t_r(raw, 2, 0), t_r(raw, 5, 3), t_imm(bits(raw, 10, 6) * 2))
	case raw >> 12 == 9: // SP-relative
		t_mem(&in_, bit(raw, 11), 4, false, t_r(raw, 10, 8), 13, t_imm((raw & 0xFF) * 4))
	case raw >> 12 == 10: // ADD rd, pc/sp, imm
		if bit(raw, 11) {
			t_dp(&in_, ADD, false, t_r(raw, 10, 8), 13, t_imm((raw & 0xFF) * 4))
		} else {
			t_dp(&in_, ADD, false, t_r(raw, 10, 8), 15, t_imm((raw & 0xFF) * 4))
			in_.align_pc = true
		}
	case raw >> 12 == 11: // misc
		decode_thumb_misc(&in_)
	case raw >> 12 == 12: // STMIA/LDMIA
		in_.kind = .Block
		in_.load = bit(raw, 11)
		in_.rn = t_r(raw, 10, 8)
		in_.reg_list = u16(raw & 0xFF)
		in_.p, in_.u = false, true
		in_.w = !(in_.load && (in_.reg_list >> uint(in_.rn)) & 1 != 0)
	case raw >> 12 == 13: // B cond, SVC
		c := bits(raw, 11, 8)
		switch c {
		case 15:
			in_.kind = .Svc
			in_.imm = raw & 0xFF
		case 14: // UDF
		case:
			in_.kind = .Branch
			in_.cond = int(c)
			in_.target = addr + 4 + u32(i32(raw << 24) >> 23)
		}
	case raw >> 11 == 0x1C: // B
		in_.kind = .Branch
		in_.target = addr + 4 + u32(i32(raw << 21) >> 20)
	case raw >> 11 == 0x1E: // BL/BLX prefix
		n := u32(next)
		hi_off := u32(i32(raw << 21) >> 9) // imm11 << 12, signed
		switch n >> 11 {
		case 0x1F: // BL
			in_.kind, in_.link, in_.len = .Branch, true, 4
			in_.target = addr + 4 + hi_off + (n & 0x7FF) << 1
			in_.raw = raw | n << 16
		case 0x1D: // BLX to ARM
			in_.kind, in_.link, in_.len, in_.exchange = .Branch, true, 4, true
			in_.target = (addr + 4 + hi_off + (n & 0x7FF) << 1) &~ 3
			in_.raw = raw | n << 16
		}
	}
	return in_
}

@(private)
decode_thumb_misc :: proc(in_: ^Inst) {
	raw := in_.raw
	switch {
	case raw >> 8 == 0xB0: // ADD/SUB sp, imm7
		op := ADD
		if bit(raw, 7) {
			op = SUB
		}
		in_.kind, in_.op, in_.rd, in_.rn = .DataProc, op, 13, 13
		in_.op2 = Operand{imm = true, value = (raw & 0x7F) * 4}
	case raw >> 8 == 0xB2: // SXTH/SXTB/UXTH/UXTB
		in_.kind = .Extend
		ops := [4]int{3, 2, 7, 6}
		in_.op = ops[bits(raw, 7, 6)]
		in_.rd, in_.rm, in_.rn = t_r(raw, 2, 0), t_r(raw, 5, 3), 15
	case raw >> 9 == 0x5A: // PUSH
		in_.kind = .Block
		in_.rn = 13
		in_.reg_list = u16(raw & 0xFF)
		if bit(raw, 8) {
			in_.reg_list |= 1 << 14
		}
		in_.p, in_.u, in_.w = true, false, true
	case raw >> 9 == 0x5E: // POP
		in_.kind = .Block
		in_.load = true
		in_.rn = 13
		in_.reg_list = u16(raw & 0xFF)
		if bit(raw, 8) {
			in_.reg_list |= 1 << 15
		}
		in_.p, in_.u, in_.w = false, true, true
	case raw >> 8 == 0xBA: // REV / REV16 / REVSH
		op := bits(raw, 7, 6)
		if op != 2 {
			in_.kind = .Rev
			ops := [4]int{0, 1, 0, 2}
			in_.op = ops[op]
			in_.rd, in_.rm = t_r(raw, 2, 0), t_r(raw, 5, 3)
		}
	case raw >> 8 == 0xBE:
		in_.kind = .Bkpt
		in_.imm = raw & 0xFF
	case raw >> 5 == 0x5B3 || raw == 0xB650 || raw == 0xB658: // CPS, SETEND
		in_.kind = .Cps
	case raw >> 8 == 0xBF && raw & 0xF == 0: // NOP-compatible hints
		in_.kind = .Hint
	}
}
