package main

import "base:intrinsics"
import "core:fmt"
import "core:hash"
import "core:os"
import "core:slice"
import "core:strings"

import "arm"

RECOMP_HEADER :: #load("runtime/recomp.h", string)

// Functions are grouped into one C file per 64 KB of code, so adding or
// changing a function rewrites only its own file; files whose content did
// not change are not touched (make rebuilds only what changed).
FILE_SPAN :: 0x10000

// put writes a file unless it already holds exactly data.
@(private = "file")
put :: proc(written: ^map[string]bool, out, name: string, data: string) -> string {
	written[strings.clone(name)] = true
	path := strings.concatenate({out, "/", name}, context.temp_allocator)
	if old, err := os.read_entire_file(path, context.temp_allocator); err == nil && string(old) == data {
		return ""
	}
	if err := os.write_entire_file(path, data); err != nil {
		return fmt.aprintf("write %s: %v", path, err)
	}
	return ""
}

sorted_keys :: proc(m: map[u32]$V, allocator := context.allocator) -> []u32 {
	keys := make([]u32, len(m), allocator)
	i := 0
	for k in m {
		keys[i] = k
		i += 1
	}
	slice.sort(keys)
	return keys
}

// generate writes text_XXXXXXXX.c files with the functions, table.c with
// the address -> function slot table, and recomp.h.
generate :: proc(p: ^Program, out: string) -> string {
	if err := os.make_directory_all(out); err != nil && !os.exists(out) {
		return fmt.aprintf("mkdir %s: %v", out, err)
	}
	written: map[string]bool
	if e := put(&written, out, "recomp.h", RECOMP_HEADER); e != "" {
		return e
	}
	addrs := sorted_keys(p.funcs)

	for i := 0; i < len(addrs); {
		span := addrs[i] / FILE_SPAN
		j := i
		for j < len(addrs) && addrs[j] / FILE_SPAN == span {
			j += 1
		}
		body := strings.builder_make()
		refs: map[u32]bool
		for a in addrs[i:j] {
			g := Gen{p = p, f = p.funcs[a], w = &body, refs = &refs}
			function(&g)
			fmt.sbprintf(&body, "RcFunc p_%08X = f_%08X;\n\n", a, a)
			free_all(context.temp_allocator)
		}
		file := strings.builder_make()
		strings.write_string(&file, "#include \"recomp.h\"\n\n")
		dyn: map[u32]bool
		for a in addrs[i:j] {
			fmt.sbprintf(&file, "void f_%08X(RcCpu* c);\n", a)
			// Entries inside a hand-written block call its dispatcher,
			// which may live in another file.
			if al := p.funcs[a].alias_of; al != 0 && !dyn[al] {
				dyn[al] = true
				fmt.sbprintf(&file, "void d_%08X(RcCpu* c, uint32_t start);\n", al)
			}
		}
		rs := sorted_keys(refs)
		for a in rs {
			fmt.sbprintf(&file, "extern RcFunc p_%08X;\n", a)
		}
		strings.write_string(&file, "\n")
		strings.write_string(&file, strings.to_string(body))
		name := fmt.tprintf("text_%08X.c", span * FILE_SPAN)
		if e := put(&written, out, name, strings.to_string(file)); e != "" {
			return e
		}
		free_all(context.temp_allocator)
		delete(rs)
		delete(dyn)
		delete(refs)
		strings.builder_destroy(&body)
		strings.builder_destroy(&file)
		i = j
	}

	// rc_addr[i] is the guest address of slot i (sorted, for rc_call's
	// search); *rc_slot[i] the function the code calls there (the runtime
	// may replace it), rc_orig[i] the generated one.
	t := strings.builder_make()
	strings.write_string(&t, "#include \"recomp.h\"\n\n")
	for a in addrs {
		fmt.sbprintf(&t, "void f_%08X(RcCpu* c); extern RcFunc p_%08X;\n", a, a)
	}
	fmt.sbprintf(&t, "\nconst unsigned rc_func_count = %d;\nconst uint32_t rc_addr[] = {{\n", len(addrs))
	for a in addrs {
		fmt.sbprintf(&t, "    0x%08X,\n", a)
	}
	strings.write_string(&t, "};\nconst RcFunc rc_orig[] = {\n")
	for a in addrs {
		fmt.sbprintf(&t, "    f_%08X,\n", a)
	}
	strings.write_string(&t, "};\nRcFunc* const rc_slot[] = {\n")
	for a in addrs {
		fmt.sbprintf(&t, "    &p_%08X,\n", a)
	}
	strings.write_string(&t, "};\n")
	// Functions enterable anywhere in their body (computed jumps).
	dyn: [dynamic]u32
	for a in addrs {
		if p.funcs[a].computed {
			append(&dyn, a)
		}
	}
	for a in dyn {
		fmt.sbprintf(&t, "void d_%08X(RcCpu* c, uint32_t start);\n", a)
	}
	fmt.sbprintf(&t, "const unsigned rc_dyn_count = %d;\nconst RcDyn rc_dyn[] = {{\n", len(dyn))
	for a in dyn {
		f := p.funcs[a]
		fmt.sbprintf(&t, "    {{0x%08X, 0x%08X, d_%08X}},\n", f.start, f.end, a)
	}
	strings.write_string(&t, "    {0, 0, 0}\n};\n")
	// The code.bin this was made from: the runtime refuses another one.
	fmt.sbprintf(&t, "const uint32_t rc_code_crc = 0x%08X;\n", hash.crc32(p.im.data))
	if e := put(&written, out, "table.c", strings.to_string(t)); e != "" {
		return e
	}
	// Remove files of an earlier generation that are no longer produced.
	if entries, err := os.read_all_directory_by_path(out, context.allocator); err == nil {
		for e in entries {
			if strings.has_suffix(e.name, ".c") && !written[e.name] {
				os.remove(strings.concatenate({out, "/", e.name}, context.temp_allocator))
			}
		}
	}
	return ""
}

Gen :: struct {
	refs:   ^map[u32]bool, // slots this file calls
	p:      ^Program,
	f:      ^Func,
	w:      ^strings.Builder,
	labels: map[u32]bool,
	order:  []u32,
	dyn:    bool,
}

@(private = "file")
out :: proc(g: ^Gen, format: string, args: ..any) { fmt.sbprintf(g.w, format, ..args) }

@(private = "file")
REG_NAMES := [16]string{
	"c->r[0]", "c->r[1]", "c->r[2]", "c->r[3]", "c->r[4]", "c->r[5]", "c->r[6]", "c->r[7]",
	"c->r[8]", "c->r[9]", "c->r[10]", "c->r[11]", "c->r[12]", "c->r[13]", "c->r[14]", "c->r[15]",
}

r :: proc(n: int) -> string {
	if n >= 0 && n < 16 {
		return REG_NAMES[n]
	}
	return fmt.tprintf("c->r[%d]", n)
}

tf :: fmt.tprintf

COND_EXPR := [15]string{"c->z", "!c->z", "c->c", "!c->c", "c->n", "!c->n", "c->v", "!c->v",
	"(c->c && !c->z)", "(!c->c || c->z)", "(c->n == c->v)", "(c->n != c->v)",
	"(!c->z && c->n == c->v)", "(c->z || c->n != c->v)", "1"}

// continues reports whether flow can go on to the next instruction.
continues :: proc(in_: arm.Inst) -> bool {
	if in_.cond != 14 {
		return true
	}
	switch {
	case in_.kind == .Unknown || in_.kind == .Bkpt:
		return false
	case in_.kind == .Branch && in_.link:
		return true
	case in_.kind == .Branch:
		return false
	case in_.kind == .BranchReg && in_.link:
		return true
	case arm.writes_pc(in_):
		return false
	case in_.kind == .Svc && (in_.imm == 0x03 || in_.imm == 0x09):
		return false
	}
	return true
}

function :: proc(g: ^Gen) {
	f := g.f
	if f.alias_of != 0 {
		out(g, "/* %08X %s: inside the hand-written block at %08X */\nvoid f_%08X(RcCpu* c) {{ d_%08X(c, 0x%08Xu); }}\n\n",
			f.entry, f.name, f.alias_of, f.entry, f.alias_of, f.entry)
		return
	}
	g.labels = make(map[u32]bool, len(f.labels) * 2 + 16, context.temp_allocator)
	for a in f.labels {
		g.labels[a] = true
	}
	g.order = sorted_keys(f.insts, context.temp_allocator)
	// Labels for fall-through that is not to the next emitted instruction.
	for a, i in g.order {
		in_ := f.insts[a]
		nx := a + in_.len
		if continues(in_) && (i + 1 >= len(g.order) || g.order[i + 1] != nx) {
			if nx in f.insts {
				g.labels[nx] = true
			}
		}
	}
	g.dyn = f.computed
	name := f.name
	if name == "" {
		name = "(entry inside a function)"
	}
	mode := "Thumb" if f.thumb else "ARM"
	if g.dyn {
		// Code with computed jumps can be entered anywhere in its body:
		// d_X(c, start) starts at any of its instructions.
		out(g, "/* %08X %s, %s, enterable anywhere in %08X-%08X */\nvoid d_%08X(RcCpu* c, uint32_t start) {{\n",
			f.entry, name, mode, f.start, f.end, f.entry)
	} else {
		out(g, "/* %08X %s, %s */\nvoid f_%08X(RcCpu* c) {{\n", f.entry, name, mode, f.entry)
	}
	// The return address the function was called with: a computed jump to it
	// (Thumb's pop {r3}; bx r3, or a moved lr) is a return.
	out(g, "    const uint32_t lr0 = c->r[14]; (void)lr0;\n")
	out(g, "    RC_ENTER(0x%08X);\n", f.entry)
	if g.dyn {
		out(g, "    uint32_t jt = start;\n    goto dispatch;\n")
	} else if len(g.order) == 0 || g.order[0] != f.entry {
		out(g, "    goto L_%08X;\n", f.entry)
	}
	for a, i in g.order {
		in_ := f.insts[a]
		if g.labels[a] {
			out(g, "L_%08X:\n", a)
		}
		inst(g, in_)
		nx := a + in_.len
		if continues(in_) && (i + 1 >= len(g.order) || g.order[i + 1] != nx) {
			switch {
			case f.insts[nx].addr == nx && g.labels[nx]:
				out(g, "    goto L_%08X;\n", nx)
			case f.fall_into[nx]:
				out(g, "    %s return;\n", slot_call(g, nx))
			case:
				out(g, "    rc_unreachable(c, 0x%08X); return;\n", nx)
			}
		}
	}
	if g.dyn {
		out(g, "dispatch:\n    switch (jt & ~1u) {{\n")
		for a in g.order {
			if g.labels[a] {
				out(g, "    case 0x%08X: goto L_%08X;\n", a, a)
			}
		}
		out(g, "    default: rc_call(c, jt); return;\n    }}\n")
	}
	out(g, "}}\n\n")
	if g.dyn {
		out(g, "void f_%08X(RcCpu* c) {{ d_%08X(c, 0x%08Xu); }}\n\n", f.entry, f.entry, f.entry)
	}
}

// slot_call calls the function at a through its slot; r15 holds the callee's
// address during the call (for overrides that serve several addresses, and
// for backtraces).
slot_call :: proc(g: ^Gen, a: u32) -> string {
	if g.refs != nil {
		g.refs[a] = true
	}
	return tf("c->r[15] = 0x%08Xu; p_%08X(c);", a, a)
}

// call emits a direct call (or tail call) to a.
call :: proc(g: ^Gen, a: u32) -> string {
	if a in g.p.funcs {
		return slot_call(g, a)
	}
	return tf("rc_call(c, 0x%08X);", a)
}

// ret returns to the caller; the address the code returns to must be the
// one the function was called with (else the runtime reports it: a longjmp
// or a corrupted stack).
ret :: proc(g: ^Gen, in_: arm.Inst, expr: string) -> string {
	if g.f.region {
		// Inside hand-written code a return may be to code of the same
		// block called by hand (add lr, pc, #n; b f).
		return jump_to(g, expr)
	}
	return tf("if (((%s) ^ lr0) > 1) rc_badreturn(c, 0x%08X, %s, lr0); RC_LEAVE(0x%08X); return;", expr, in_.addr, expr, in_.addr)
}

// jump_to emits a jump to a computed address held in expr.
jump_to :: proc(g: ^Gen, expr: string) -> string {
	if g.dyn {
		return tf("{{ uint32_t t = %s; if ((t ^ lr0) <= 1) return; jt = t; goto dispatch; }}", expr)
	}
	return tf("{{ uint32_t t = %s; if ((t ^ lr0) <= 1) return; rc_call(c, t); return; }}", expr)
}

// reg reads register n as an operand of in (r15 reads as the PC value).
reg :: proc(in_: arm.Inst, n: int) -> string {
	if n == 15 {
		return tf("0x%08Xu", arm.pc(in_))
	}
	return r(n)
}

// op2 returns the C expression of a shifter operand and its carry out
// ("" when the carry flag is unchanged). Register-shifted operands need a
// statement; pre holds it.
op2 :: proc(in_: arm.Inst, o: arm.Operand) -> (expr, carry, pre: string) {
	if o.imm {
		if o.rot != 0 {
			carry = tf("%du", o.value >> 31)
		}
		return tf("0x%08Xu", o.value), carry, ""
	}
	m := reg(in_, o.rm)
	if o.reg_shift {
		pre = tf("uint32_t sc; uint32_t sv = rc_shift_c(c->c, %s, %d, %s, &sc);", m, o.shift, reg(in_, o.rs))
		return "sv", "sc", pre
	}
	n := o.amount
	switch o.shift {
	case arm.LSL:
		if n == 0 {
			return m, "", ""
		}
		return tf("(%s << %d)", m, n), tf("((%s >> %d) & 1)", m, 32 - n), ""
	case arm.LSR:
		if n == 32 {
			return "0u", tf("(%s >> 31)", m), ""
		}
		return tf("(%s >> %d)", m, n), tf("((%s >> %d) & 1)", m, n - 1), ""
	case arm.ASR:
		if n == 32 {
			return tf("(uint32_t)((int32_t)%s >> 31)", m), tf("(%s >> 31)", m), ""
		}
		return tf("(uint32_t)((int32_t)%s >> %d)", m, n), tf("((%s >> %d) & 1)", m, n - 1), ""
	case arm.ROR:
		return tf("rc_ror(%s, %d)", m, n), tf("((%s >> %d) & 1)", m, n - 1), ""
	case: // RRX
		return tf("((c->c << 31) | (%s >> 1))", m), tf("(%s & 1)", m), ""
	}
}

inst :: proc(g: ^Gen, in_: arm.Inst) {
	b := body(g, in_)
	if in_.cond != 14 && in_.kind != .Unknown {
		out(g, "    if (%s) {{ %s }}\n", COND_EXPR[in_.cond], b)
	} else {
		out(g, "    %s\n", b)
	}
}

ret_addr :: proc(in_: arm.Inst) -> u32 {
	a := in_.addr + in_.len
	if in_.thumb {
		a |= 1
	}
	return a
}

body :: proc(g: ^Gen, in_: arm.Inst) -> string {
	#partial switch in_.kind {
	case .DataProc:
		return data_proc(g, in_)
	case .Mem:
		return mem(g, in_)
	case .Block:
		return block(g, in_)
	case .Branch:
		return branch(g, in_)
	case .BranchReg:
		if in_.link {
			return tf("{{ uint32_t t = %s; c->r[14] = 0x%08Xu; rc_call(c, t); }}", reg(in_, in_.rm), ret_addr(in_))
		}
		if in_.rm == 14 {
			return ret(g, in_, "c->r[14]")
		}
		return jump_to(g, reg(in_, in_.rm))
	case .Mul:
		return mul(in_)
	case .MulLong:
		return mul_long(in_)
	case .MulHalf:
		return mul_half(in_)
	case .MulDual:
		return mul_dual(in_)
	case .Clz:
		return tf("%s = rc_clz(%s);", r(in_.rd), reg(in_, in_.rm))
	case .Sat:
		a, b := reg(in_, in_.rm), reg(in_, in_.rn)
		switch in_.op {
		case 0:
			return tf("%s = (uint32_t)rc_sat32(c, (int64_t)(int32_t)%s + (int32_t)%s);", r(in_.rd), a, b)
		case 1:
			return tf("%s = (uint32_t)rc_sat32(c, (int64_t)(int32_t)%s - (int32_t)%s);", r(in_.rd), a, b)
		case 2:
			return tf("%s = (uint32_t)rc_sat32(c, (int64_t)(int32_t)%s + rc_sat32(c, 2 * (int64_t)(int32_t)%s));", r(in_.rd), a, b)
		case:
			return tf("%s = (uint32_t)rc_sat32(c, (int64_t)(int32_t)%s - rc_sat32(c, 2 * (int64_t)(int32_t)%s));", r(in_.rd), a, b)
		}
	case .Mrs:
		return tf("%s = rc_cpsr(c);", r(in_.rd))
	case .Msr:
		if in_.op == 1 {
			return "/* msr spsr */"
		}
		v, _, _ := op2(in_, in_.op2)
		return tf("rc_set_cpsr(c, %s, %d);", v, in_.mask)
	case .Clrex:
		return "c->excl_addr = RC_NO_EXCL;"
	case .Hint, .Pld, .Cps:
		return ";"
	case .Svc:
		if in_.imm == 0x03 || in_.imm == 0x09 {
			return tf("rc_svc(c, 0x%X); return;", in_.imm)
		}
		return tf("rc_svc(c, 0x%X);", in_.imm)
	case .Bkpt:
		return tf("rc_bkpt(c, 0x%08X, 0x%X); return;", in_.addr, in_.imm)
	case .Extend:
		return extend(in_)
	case .Pkh:
		if in_.op == 0 {
			return tf("%s = (%s & 0xFFFFu) | ((%s << %d) & 0xFFFF0000u);", r(in_.rd), reg(in_, in_.rn), reg(in_, in_.rm), in_.shift)
		}
		sh := in_.shift
		if sh == 32 {
			sh = 31
		}
		return tf("%s = (%s & 0xFFFF0000u) | ((uint32_t)((int32_t)%s >> %d) & 0xFFFFu);", r(in_.rd), reg(in_, in_.rn), reg(in_, in_.rm), sh)
	case .Ssat:
		v := reg(in_, in_.rm)
		if in_.shift_t == arm.ASR {
			sh := in_.shift
			if sh == 32 {
				sh = 31
			}
			v = tf("(uint32_t)((int32_t)%s >> %d)", v, sh)
		} else if in_.shift != 0 {
			v = tf("(%s << %d)", v, in_.shift)
		}
		if in_.op == 0 {
			return tf("%s = rc_ssat(c, (int32_t)%s, %d);", r(in_.rd), v, in_.imm + 1)
		}
		return tf("%s = rc_usat(c, (int32_t)%s, %d);", r(in_.rd), v, in_.imm)
	case .Sat16:
		fn := "rc_ssat"
		n := in_.imm + 1
		if in_.op == 1 {
			fn, n = "rc_usat", in_.imm
		}
		m := reg(in_, in_.rm)
		return tf("%s = (%s(c, (int16_t)%s, %d) & 0xFFFFu) | (%s(c, (int16_t)(%s >> 16), %d) << 16);", r(in_.rd), fn, m, n, fn, m, n)
	case .Rev:
		m := reg(in_, in_.rm)
		switch in_.op {
		case 0:
			return tf("%s = __builtin_bswap32(%s);", r(in_.rd), m)
		case 1:
			return tf("{{ uint32_t t = %s; %s = ((t & 0x00FF00FFu) << 8) | ((t >> 8) & 0x00FF00FFu); }}", m, r(in_.rd))
		case:
			return tf("%s = (uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t)%s);", r(in_.rd), m)
		}
	case .Sel:
		return tf("%s = rc_sel(c, %s, %s);", r(in_.rd), reg(in_, in_.rn), reg(in_, in_.rm))
	case .Parallel:
		return tf("%s = rc_parallel(c, %d, %s, %s);", r(in_.rd), in_.op, reg(in_, in_.rn), reg(in_, in_.rm))
	case .MemEx:
		return mem_ex(in_)
	case .Swp:
		ld, st := "ld32", "st32"
		if in_.size == 1 {
			ld, st = "ld8", "st8"
		}
		return tf("{{ uint32_t a = %s, t = %s(a); %s(a, %s); %s = t; }}", reg(in_, in_.rn), ld, st, reg(in_, in_.rm), r(in_.rd))
	case .CoprocReg:
		if in_.cp != 15 {
			return tf("rc_trap(c, 0x%08X, 0x%08X); return;", in_.addr, in_.raw)
		}
		if in_.load {
			if in_.crn == 13 && in_.crm == 0 && in_.opc2 == 3 {
				return tf("%s = c->tls;", r(in_.rd))
			}
			return tf("%s = rc_cp15_read(c, 0x%08X, 0x%08X);", r(in_.rd), in_.addr, in_.raw)
		}
		return tf("rc_cp15_write(c, 0x%08X, 0x%08X, %s);", in_.addr, in_.raw, reg(in_, in_.rd))
	case .VfpArith:
		return vfp_arith(in_)
	case .VfpCmp:
		if in_.dbl {
			if in_.zero {
				return tf("rc_fcmp(c, c->f.d[%d], 0.0);", in_.vd)
			}
			return tf("rc_fcmp(c, c->f.d[%d], c->f.d[%d]);", in_.vd, in_.vm)
		}
		if in_.zero {
			return tf("rc_fcmp(c, c->f.s[%d], 0.0);", in_.vd)
		}
		return tf("rc_fcmp(c, c->f.s[%d], c->f.s[%d]);", in_.vd, in_.vm)
	case .VfpCvt:
		return vfp_cvt(in_)
	case .VfpMovCore:
		if in_.load {
			return tf("%s = c->f.w[%d];", r(in_.rd), in_.vn)
		}
		return tf("c->f.w[%d] = %s;", in_.vn, reg(in_, in_.rd))
	case .VfpMovHalf:
		k := 2 * in_.vn
		if in_.x {
			k += 1
		}
		if in_.load {
			return tf("%s = c->f.w[%d];", r(in_.rd), k)
		}
		return tf("c->f.w[%d] = %s;", k, reg(in_, in_.rd))
	case .VfpMov2:
		a, b := in_.vm, in_.vm + 1
		if in_.dbl {
			a, b = 2 * in_.vm, 2 * in_.vm + 1
		}
		if in_.load {
			return tf("%s = c->f.w[%d]; %s = c->f.w[%d];", r(in_.rd), a, r(in_.rn), b)
		}
		return tf("c->f.w[%d] = %s; c->f.w[%d] = %s;", a, reg(in_, in_.rd), b, reg(in_, in_.rn))
	case .VfpSys:
		if in_.load {
			switch {
			case in_.reg == 1 && in_.rd == 15:
				return "rc_fpscr_to_flags(c);"
			case in_.reg == 1:
				return tf("%s = c->fpscr;", r(in_.rd))
			case in_.reg == 8:
				return tf("%s = 0x40000000u;", r(in_.rd)) // FPEXC: enabled
			case:
				return tf("%s = 0x410120B4u;", r(in_.rd)) // FPSID (VFP11)
			}
		}
		if in_.reg == 1 {
			return tf("c->fpscr = %s;", reg(in_, in_.rd))
		}
		return ";"
	case .VfpMem:
		return vfp_mem(in_)
	case .VfpBlock:
		return vfp_block(in_)
	}
	return tf("rc_trap(c, 0x%08X, 0x%08X); return;", in_.addr, in_.raw)
}

data_proc :: proc(g: ^Gen, in_: arm.Inst) -> string {
	logical := in_.op == arm.AND || in_.op == arm.EOR || in_.op == arm.TST || in_.op == arm.TEQ ||
		in_.op == arm.ORR || in_.op == arm.MOV || in_.op == arm.BIC || in_.op == arm.MVN
	b, carry, pre := op2(in_, in_.op2)
	a := ""
	if in_.op != arm.MOV && in_.op != arm.MVN {
		a = reg(in_, in_.rn)
	}
	e: string
	switch in_.op {
	case arm.AND, arm.TST:
		e = tf("%s & %s", a, b)
	case arm.EOR, arm.TEQ:
		e = tf("%s ^ %s", a, b)
	case arm.ORR:
		e = tf("%s | %s", a, b)
	case arm.BIC:
		e = tf("%s & ~%s", a, b)
	case arm.MOV:
		e = b
	case arm.MVN:
		e = tf("~%s", b)
	case arm.ADD, arm.CMN:
		e = tf("rc_add(c, %s, %s, 0)", a, b) if in_.s else tf("%s + %s", a, b)
	case arm.SUB, arm.CMP:
		e = tf("rc_sub(c, %s, %s, 1)", a, b) if in_.s else tf("%s - %s", a, b)
	case arm.RSB:
		e = tf("rc_sub(c, %s, %s, 1)", b, a) if in_.s else tf("%s - %s", b, a)
	case arm.ADC:
		e = tf("rc_add(c, %s, %s, c->c)", a, b) if in_.s else tf("%s + %s + c->c", a, b)
	case arm.SBC:
		e = tf("rc_sub(c, %s, %s, c->c)", a, b) if in_.s else tf("%s - %s - !c->c", a, b)
	case arm.RSC:
		e = tf("rc_sub(c, %s, %s, c->c)", b, a) if in_.s else tf("%s - %s - !c->c", b, a)
	}
	// Jump tables: add pc, pc, rX, lsl #2.
	if t, ok := g.f.tables[in_.addr]; ok {
		return table(g, in_, t, reg(in_, in_.op2.rm))
	}
	s := strings.builder_make(context.temp_allocator)
	strings.write_string(&s, "{ ")
	if pre != "" {
		strings.write_string(&s, pre)
		strings.write_string(&s, " ")
	}
	strings.write_string(&s, "uint32_t res = ")
	strings.write_string(&s, e)
	strings.write_string(&s, "; ")
	if in_.s && logical {
		strings.write_string(&s, "rc_nz(c, res); ")
		if carry != "" {
			strings.write_string(&s, "c->c = ")
			strings.write_string(&s, carry)
			strings.write_string(&s, "; ")
		}
	}
	test := in_.op >= arm.TST && in_.op <= arm.CMN
	if !test {
		if in_.rd == 15 {
			if arm.is_return(in_) {
				strings.write_string(&s, ret(g, in_, "res"))
			} else {
				strings.write_string(&s, jump_to(g, "res"))
			}
			strings.write_string(&s, " ")
		} else {
			strings.write_string(&s, r(in_.rd))
			strings.write_string(&s, " = res; ")
		}
	} else {
		strings.write_string(&s, "(void)res; ")
	}
	strings.write_string(&s, "}")
	return strings.to_string(s)
}

table :: proc(g: ^Gen, in_: arm.Inst, t: []u32, idx: string) -> string {
	s := strings.builder_make(context.temp_allocator)
	fmt.sbprintf(&s, "switch (%s) {{", idx)
	for a, k in t {
		if g.f.insts[a].addr == a && a >= g.f.start && a < g.f.end {
			g.labels[a] = true
			fmt.sbprintf(&s, " case %d: goto L_%08X;", k, a)
		} else {
			fmt.sbprintf(&s, " case %d: %s return;", k, call(g, a))
		}
	}
	fmt.sbprintf(&s, " default: rc_unreachable(c, 0x%08X); return; }}", in_.addr)
	return strings.to_string(s)
}

// off_expr is the offset expression of a load/store.
off_expr :: proc(in_: arm.Inst) -> string {
	o := in_.off
	if o.imm {
		return tf("0x%Xu", o.value)
	}
	e, _, _ := op2(in_, arm.Operand{rm = o.rm, shift = o.shift, amount = o.amount})
	return e
}

@(private = "file")
ld_name :: proc(size: int, signed: bool) -> string {
	if signed {
		switch size {
		case 2: return "lds16"
		case 1: return "lds8"
		}
		return ""
	}
	switch size {
	case 4: return "ld32"
	case 2: return "ld16"
	case 1: return "ld8"
	}
	return ""
}

@(private = "file")
st_name :: proc(size: int) -> string {
	switch size {
	case 4: return "st32"
	case 2: return "st16"
	case 1: return "st8"
	}
	return ""
}

mem :: proc(g: ^Gen, in_: arm.Inst) -> string {
	if t, ok := g.f.tables[in_.addr]; ok {
		return table(g, in_, t, r(in_.off.rm))
	}
	general := false
	// Literal from read-only memory: a constant.
	if in_.load && in_.rn == 15 && in_.off.imm && in_.p && !in_.w && in_.rd != 15 {
		a := arm.pc(in_) - in_.off.value
		if in_.u {
			a = arm.pc(in_) + in_.off.value
		}
		if read_only(g.p.im, a) && read_only(g.p.im, a + u32(in_.size) - 1) {
			w, _ := word(g.p.im, a &~ 3)
			w2, _ := word(g.p.im, (a &~ 3) + 4)
			v := u64(w) | u64(w2) << 32
			v >>= 8 * u64(a & 3)
			x: u32
			switch {
			case in_.size == 4:
				x = u32(v)
			case in_.size == 2 && in_.signed:
				x = u32(i32(i16(v)))
			case in_.size == 2:
				x = u32(u16(v))
			case in_.size == 1 && in_.signed:
				x = u32(i32(i8(v)))
			case in_.size == 1:
				x = u32(u8(v))
			case:
				general = true
			}
			if !general {
				return tf("%s = 0x%08Xu;", r(in_.rd), x)
			}
		}
	}
	if !general {
		if t, ok := const_jump(g.p, in_); ok {
			return tf("%s return;", call(g, t &~ 1))
		}
	}
	base := reg(in_, in_.rn)
	off := off_expr(in_)
	sign := "+" if in_.u else "-"
	s := strings.builder_make(context.temp_allocator)
	strings.write_string(&s, "{ ")
	if in_.p {
		fmt.sbprintf(&s, "uint32_t a = %s %s %s; ", base, sign, off)
	} else {
		fmt.sbprintf(&s, "uint32_t a = %s; ", base)
	}
	wb := ""
	if in_.w || !in_.p {
		if in_.p {
			wb = tf("%s = a; ", r(in_.rn))
		} else {
			wb = tf("%s = a %s %s; ", r(in_.rn), sign, off)
		}
	}
	if in_.load {
		switch in_.size {
		case 8:
			fmt.sbprintf(&s, "uint32_t v0 = ld32(a), v1 = ld32(a + 4); %s%s = v0; %s = v1; ", wb, r(in_.rd), r(in_.rd + 1))
		case:
			fmt.sbprintf(&s, "uint32_t v = %s(a); %s", ld_name(in_.size, in_.signed), wb)
			if in_.rd == 15 {
				if arm.is_return(in_) {
					strings.write_string(&s, ret(g, in_, "v"))
				} else {
					strings.write_string(&s, jump_to(g, "v"))
				}
				strings.write_string(&s, " ")
			} else {
				fmt.sbprintf(&s, "%s = v; ", r(in_.rd))
			}
		}
	} else {
		switch in_.size {
		case 8:
			fmt.sbprintf(&s, "st32(a, %s); st32(a + 4, %s); %s", reg(in_, in_.rd), reg(in_, in_.rd + 1), wb)
		case:
			v := reg(in_, in_.rd)
			if in_.rd == 15 {
				v = tf("0x%08Xu", in_.addr + 12)
			}
			fmt.sbprintf(&s, "%s(a, %s); %s", st_name(in_.size), v, wb)
		}
	}
	strings.write_string(&s, "}")
	return strings.to_string(s)
}

mem_ex :: proc(in_: arm.Inst) -> string {
	a := reg(in_, in_.rn)
	if in_.load {
		switch in_.size {
		case 8:
			return tf("rc_ldrexd(c, %s, &%s, &%s);", a, r(in_.rd), r(in_.rd + 1))
		case 4:
			return tf("%s = rc_ldrex32(c, %s);", r(in_.rd), a)
		case 2:
			return tf("%s = rc_ldrex16(c, %s);", r(in_.rd), a)
		case:
			return tf("%s = rc_ldrex8(c, %s);", r(in_.rd), a)
		}
	}
	// STREX Rd, Rm, [Rn]: a compare-and-swap against what LDREX read.
	switch in_.size {
	case 8:
		return tf("%s = rc_strexd(c, %s, %s, %s);", r(in_.rd), a, reg(in_, in_.rm), reg(in_, in_.rm + 1))
	case 4:
		return tf("%s = rc_strex32(c, %s, %s);", r(in_.rd), a, reg(in_, in_.rm))
	case 2:
		return tf("%s = rc_strex16(c, %s, %s);", r(in_.rd), a, reg(in_, in_.rm))
	case:
		return tf("%s = rc_strex8(c, %s, %s);", r(in_.rd), a, reg(in_, in_.rm))
	}
}

block :: proc(g: ^Gen, in_: arm.Inst) -> string {
	n := u32(intrinsics.count_ones(in_.reg_list))
	start: string
	base := reg(in_, in_.rn)
	switch {
	case !in_.p && in_.u: // IA
		start = base
	case in_.p && in_.u: // IB
		start = tf("%s + 4", base)
	case !in_.p && !in_.u: // DA
		start = tf("%s - %d", base, 4 * n - 4)
	case: // DB
		start = tf("%s - %d", base, 4 * n)
	}
	s := strings.builder_make(context.temp_allocator)
	fmt.sbprintf(&s, "{{ uint32_t a = %s; ", start)
	wb_expr := ""
	if in_.w {
		if in_.u {
			wb_expr = tf("%s = %s + %d; ", r(in_.rn), base, 4 * n)
		} else {
			wb_expr = tf("%s = %s - %d; ", r(in_.rn), base, 4 * n)
		}
	}
	k := 0
	if in_.load {
		// Load into temporaries first: the base may be in the list.
		for i in 0 ..< 16 {
			if (in_.reg_list >> uint(i)) & 1 != 0 {
				fmt.sbprintf(&s, "uint32_t t%d = ld32(a + %d); ", i, 4 * k)
				k += 1
			}
		}
		if in_.w && (in_.reg_list >> uint(in_.rn)) & 1 == 0 {
			strings.write_string(&s, wb_expr)
		}
		for i in 0 ..< 15 {
			if (in_.reg_list >> uint(i)) & 1 != 0 {
				fmt.sbprintf(&s, "%s = t%d; ", r(i), i)
			}
		}
		if in_.reg_list & 0x8000 != 0 {
			if arm.is_return(in_) {
				strings.write_string(&s, ret(g, in_, "t15"))
			} else {
				strings.write_string(&s, jump_to(g, "t15"))
			}
			strings.write_string(&s, " ")
		}
	} else {
		for i in 0 ..< 16 {
			if (in_.reg_list >> uint(i)) & 1 != 0 {
				v := r(i)
				if i == 15 {
					v = tf("0x%08Xu", in_.addr + 12)
				}
				fmt.sbprintf(&s, "st32(a + %d, %s); ", 4 * k, v)
				k += 1
			}
		}
		strings.write_string(&s, wb_expr)
	}
	strings.write_string(&s, "}")
	return strings.to_string(s)
}

branch :: proc(g: ^Gen, in_: arm.Inst) -> string {
	if in_.link {
		return tf("c->r[14] = 0x%08Xu; %s", ret_addr(in_), call(g, in_.target))
	}
	if in_.target >= g.f.start && in_.target < g.f.end {
		if in_.target in g.f.insts {
			if in_.target <= in_.addr {
				// A loop: let a higher-priority thread that became ready run.
				return tf("RC_POLL(); goto L_%08X;", in_.target)
			}
			return tf("goto L_%08X;", in_.target)
		}
	}
	return tf("%s return;", call(g, in_.target))
}

mul :: proc(in_: arm.Inst) -> string {
	e: string
	switch in_.op {
	case 1:
		e = tf("%s * %s + %s", reg(in_, in_.rm), reg(in_, in_.rs), reg(in_, in_.rn))
	case 2:
		e = tf("%s - %s * %s", reg(in_, in_.rn), reg(in_, in_.rm), reg(in_, in_.rs))
	case:
		e = tf("%s * %s", reg(in_, in_.rm), reg(in_, in_.rs))
	}
	if in_.s {
		return tf("{{ uint32_t res = %s; rc_nz(c, res); %s = res; }}", e, r(in_.rd))
	}
	return tf("%s = %s;", r(in_.rd), e)
}

mul_long :: proc(in_: arm.Inst) -> string {
	hi, lo := in_.rd, in_.rn
	m, s := reg(in_, in_.rm), reg(in_, in_.rs)
	e: string
	switch in_.op {
	case 0:
		e = tf("(uint64_t)%s * %s", m, s)
	case 1:
		e = tf("(uint64_t)%s * %s + ((uint64_t)%s << 32 | %s)", m, s, r(hi), r(lo))
	case 2:
		e = tf("(uint64_t)((int64_t)(int32_t)%s * (int32_t)%s)", m, s)
	case 3:
		e = tf("(uint64_t)((int64_t)(int32_t)%s * (int32_t)%s) + ((uint64_t)%s << 32 | %s)", m, s, r(hi), r(lo))
	case: // UMAAL
		e = tf("(uint64_t)%s * %s + %s + %s", m, s, r(hi), r(lo))
	}
	flags := ""
	if in_.s {
		flags = "c->n = (uint32_t)(x >> 63); c->z = x == 0; "
	}
	return tf("{{ uint64_t x = %s; %s%s = (uint32_t)x; %s = (uint32_t)(x >> 32); }}", e, flags, r(lo), r(hi))
}

half_of :: proc(v: string, top: bool) -> string {
	if top {
		return tf("(int32_t)(int16_t)(%s >> 16)", v)
	}
	return tf("(int32_t)(int16_t)%s", v)
}

mul_half :: proc(in_: arm.Inst) -> string {
	m, s := reg(in_, in_.rm), reg(in_, in_.rs)
	switch in_.op {
	case 0: // SMLAxy
		// Wraps; Q records the overflow.
		return tf("{{ int64_t x = (int64_t)(%s * %s) + (int32_t)%s; if (x != (int32_t)x) c->q = 1; %s = (uint32_t)x; }}", half_of(m, in_.x), half_of(s, in_.y), reg(in_, in_.rn), r(in_.rd))
	case 3: // SMULxy
		return tf("%s = (uint32_t)(%s * %s);", r(in_.rd), half_of(m, in_.x), half_of(s, in_.y))
	case 1: // SMLAWy
		return tf("{{ int64_t x = (((int64_t)(int32_t)%s * %s) >> 16) + (int32_t)%s; if (x != (int32_t)x) c->q = 1; %s = (uint32_t)x; }}", m, half_of(s, in_.y), reg(in_, in_.rn), r(in_.rd))
	case 4: // SMULWy
		return tf("%s = (uint32_t)(((int64_t)(int32_t)%s * %s) >> 16);", r(in_.rd), m, half_of(s, in_.y))
	case: // SMLALxy: RdHi = Rd, RdLo = Rn
		return tf("{{ int64_t x = (int64_t)((uint64_t)%s << 32 | %s) + (int64_t)(%s * %s); %s = (uint32_t)x; %s = (uint32_t)((uint64_t)x >> 32); }}",
			r(in_.rd), r(in_.rn), half_of(m, in_.x), half_of(s, in_.y), r(in_.rn), r(in_.rd))
	}
}

mul_dual :: proc(in_: arm.Inst) -> string {
	n, m := reg(in_, in_.rm), reg(in_, in_.rs)
	if in_.x {
		m = tf("rc_ror(%s, 16)", m)
	}
	p1 := tf("((int64_t)%s * %s)", half_of(n, false), half_of(m, false))
	p2 := tf("((int64_t)%s * %s)", half_of(n, true), half_of(m, true))
	acc := in_.rn != 15
	switch in_.op {
	case 0, 1:
		op := "-" if in_.op == 1 else "+"
		e := tf("%s %s %s", p1, op, p2)
		if acc {
			return tf("{{ int64_t x = %s + (int32_t)%s; if (x != (int32_t)x) c->q = 1; %s = (uint32_t)x; }}", e, r(in_.rn), r(in_.rd))
		}
		return tf("{{ int64_t x = %s; if (x != (int32_t)x) c->q = 1; %s = (uint32_t)x; }}", e, r(in_.rd))
	case 2, 3: // SMLALD / SMLSLD: RdHi = Rd, RdLo = Rn
		op := "-" if in_.op == 3 else "+"
		return tf("{{ int64_t x = (int64_t)((uint64_t)%s << 32 | %s) + %s %s %s; %s = (uint32_t)x; %s = (uint32_t)((uint64_t)x >> 32); }}",
			r(in_.rd), r(in_.rn), p1, op, p2, r(in_.rn), r(in_.rd))
	case 4, 5: // SMMLA / SMMUL / SMMLS (R = bit 5 rounds)
		round := ""
		if (in_.raw >> 5) & 1 != 0 {
			round = " + 0x80000000LL"
		}
		prod := tf("(int64_t)(int32_t)%s * (int32_t)%s", reg(in_, in_.rm), reg(in_, in_.rs))
		if in_.op == 4 {
			if acc {
				return tf("%s = (uint32_t)((((int64_t)%s << 32) + %s%s) >> 32);", r(in_.rd), r(in_.rn), prod, round)
			}
			return tf("%s = (uint32_t)((%s%s) >> 32);", r(in_.rd), prod, round)
		}
		return tf("%s = (uint32_t)((((int64_t)%s << 32) - %s%s) >> 32);", r(in_.rd), r(in_.rn), prod, round)
	case: // USAD8 / USADA8
		s := strings.builder_make(context.temp_allocator)
		fmt.sbprintf(&s, "{{ uint32_t x = 0, a = %s, b = %s; for (int i = 0; i < 32; i += 8) {{ int d = (int)((a >> i) & 255) - (int)((b >> i) & 255); x += (uint32_t)(d < 0 ? -d : d); }} ", reg(in_, in_.rm), reg(in_, in_.rs))
		if acc {
			fmt.sbprintf(&s, "x += %s; ", r(in_.rn))
		}
		fmt.sbprintf(&s, "%s = x; }}", r(in_.rd))
		return strings.to_string(s)
	}
}

extend :: proc(in_: arm.Inst) -> string {
	m := reg(in_, in_.rm)
	if in_.rot != 0 {
		m = tf("rc_ror(%s, %d)", m, in_.rot)
	}
	e: string
	switch in_.op {
	case 0: // SXTB16
		e = tf("(((uint32_t)(int32_t)(int8_t)%s & 0xFFFFu) | ((uint32_t)(int32_t)(int8_t)(%s >> 16) << 16))", m, m)
	case 2:
		e = tf("(uint32_t)(int32_t)(int8_t)%s", m)
	case 3:
		e = tf("(uint32_t)(int32_t)(int16_t)%s", m)
	case 4: // UXTB16
		e = tf("(%s & 0x00FF00FFu)", m)
	case 6:
		e = tf("(%s & 0xFFu)", m)
	case:
		e = tf("(%s & 0xFFFFu)", m)
	}
	if in_.rn != 15 {
		if in_.op == 0 || in_.op == 4 { // 16-bit lanes add separately
			return tf("{{ uint32_t x = %s, y = %s; %s = ((y + x) & 0xFFFFu) | (((y >> 16) + (x >> 16)) << 16); }}", e, r(in_.rn), r(in_.rd))
		}
		e = tf("%s + %s", r(in_.rn), e)
	}
	return tf("%s = %s;", r(in_.rd), e)
}

vfp_arith :: proc(in_: arm.Inst) -> string {
	R :: proc(dbl: bool, i: int) -> string {
		if dbl {
			return tf("c->f.d[%d]", i)
		}
		return tf("c->f.s[%d]", i)
	}
	d, n, m := R(in_.dbl, in_.vd), R(in_.dbl, in_.vn), R(in_.dbl, in_.vm)
	sq := "sqrt" if in_.dbl else "sqrtf"
	ab := "fabs" if in_.dbl else "fabsf"
	switch in_.op {
	case arm.VMLA:
		return tf("%s = %s + %s * %s;", d, d, n, m)
	case arm.VMLS:
		return tf("%s = %s - %s * %s;", d, d, n, m)
	case arm.VNMLS:
		return tf("%s = -%s + %s * %s;", d, d, n, m)
	case arm.VNMLA:
		return tf("%s = -%s - %s * %s;", d, d, n, m)
	case arm.VMUL:
		return tf("%s = %s * %s;", d, n, m)
	case arm.VNMUL:
		return tf("%s = -(%s * %s);", d, n, m)
	case arm.VADD:
		return tf("%s = %s + %s;", d, n, m)
	case arm.VSUB:
		return tf("%s = %s - %s;", d, n, m)
	case arm.VDIV:
		return tf("%s = %s / %s;", d, n, m)
	case arm.VMOV:
		if in_.dbl {
			return tf("c->f.w[%d] = c->f.w[%d]; c->f.w[%d] = c->f.w[%d];", 2 * in_.vd, 2 * in_.vm, 2 * in_.vd + 1, 2 * in_.vm + 1)
		}
		return tf("c->f.w[%d] = c->f.w[%d];", in_.vd, in_.vm)
	case arm.VABS:
		return tf("%s = %s(%s);", d, ab, m)
	case arm.VNEG:
		if in_.dbl {
			return tf("c->f.w[%d] = c->f.w[%d]; c->f.w[%d] = c->f.w[%d] ^ 0x80000000u;", 2 * in_.vd, 2 * in_.vm, 2 * in_.vd + 1, 2 * in_.vm + 1)
		}
		return tf("c->f.w[%d] = c->f.w[%d] ^ 0x80000000u;", in_.vd, in_.vm)
	case:
		return tf("%s = %s(%s);", d, sq, m)
	}
}

vfp_cvt :: proc(in_: arm.Inst) -> string {
	S :: proc(i: int) -> string { return tf("c->f.s[%d]", i) }
	D :: proc(i: int) -> string { return tf("c->f.d[%d]", i) }
	W :: proc(i: int) -> string { return tf("c->f.w[%d]", i) }
	rnd :: proc(e: bool, x: string) -> string {
		if e {
			return tf("rc_round_mode(c, %s)", x)
		}
		return x
	}
	switch in_.op {
	case arm.CVT_F64_F32:
		return tf("%s = (double)%s;", D(in_.vd), S(in_.vm))
	case arm.CVT_F32_F64:
		return tf("%s = (float)%s;", S(in_.vd), D(in_.vm))
	case arm.CVT_F32_U32:
		return tf("%s = (float)%s;", S(in_.vd), W(in_.vm))
	case arm.CVT_F32_S32:
		return tf("%s = (float)(int32_t)%s;", S(in_.vd), W(in_.vm))
	case arm.CVT_F64_U32:
		return tf("%s = (double)%s;", D(in_.vd), W(in_.vm))
	case arm.CVT_F64_S32:
		return tf("%s = (double)(int32_t)%s;", D(in_.vd), W(in_.vm))
	case arm.CVT_U32_F32:
		return tf("%s = rc_f2u(%s);", W(in_.vd), rnd(in_.e, tf("(double)%s", S(in_.vm))))
	case arm.CVT_S32_F32:
		return tf("%s = rc_f2s(%s);", W(in_.vd), rnd(in_.e, tf("(double)%s", S(in_.vm))))
	case arm.CVT_U32_F64:
		return tf("%s = rc_f2u(%s);", W(in_.vd), rnd(in_.e, D(in_.vm)))
	case:
		return tf("%s = rc_f2s(%s);", W(in_.vd), rnd(in_.e, D(in_.vm)))
	}
}

vfp_mem :: proc(in_: arm.Inst) -> string {
	base := reg(in_, in_.rn)
	if in_.rn == 15 {
		base = tf("0x%08Xu", arm.pc(in_) &~ 3)
	}
	sign := "+" if in_.u else "-"
	a := tf("%s %s %d", base, sign, in_.imm)
	if in_.dbl {
		if in_.load {
			return tf("{{ uint32_t a = %s; c->f.w[%d] = ld32(a); c->f.w[%d] = ld32(a + 4); }}", a, 2 * in_.vd, 2 * in_.vd + 1)
		}
		return tf("{{ uint32_t a = %s; st32(a, c->f.w[%d]); st32(a + 4, c->f.w[%d]); }}", a, 2 * in_.vd, 2 * in_.vd + 1)
	}
	if in_.load {
		return tf("c->f.w[%d] = ld32(%s);", in_.vd, a)
	}
	return tf("st32(%s, c->f.w[%d]);", a, in_.vd)
}

vfp_block :: proc(in_: arm.Inst) -> string {
	words := in_.count
	first := in_.vd
	if in_.dbl {
		words = in_.count * 2
		first = in_.vd * 2
	}
	nbytes := 4 * words
	if in_.dbl && in_.op == 1 { // FLDMX/FSTMX: one extra word
		nbytes += 4
	}
	base := reg(in_, in_.rn)
	s := strings.builder_make(context.temp_allocator)
	if in_.p { // decrement before
		fmt.sbprintf(&s, "{{ uint32_t a = %s - %d; ", base, nbytes)
	} else {
		fmt.sbprintf(&s, "{{ uint32_t a = %s; ", base)
	}
	for k in 0 ..< words {
		if in_.load {
			fmt.sbprintf(&s, "c->f.w[%d] = ld32(a + %d); ", first + k, 4 * k)
		} else {
			fmt.sbprintf(&s, "st32(a + %d, c->f.w[%d]); ", 4 * k, first + k)
		}
	}
	if in_.w {
		if in_.u {
			fmt.sbprintf(&s, "%s = %s + %d; ", r(in_.rn), base, nbytes)
		} else {
			fmt.sbprintf(&s, "%s = %s - %d; ", r(in_.rn), base, nbytes)
		}
	}
	strings.write_string(&s, "}")
	return strings.to_string(s)
}
