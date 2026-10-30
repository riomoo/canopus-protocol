package main

import (
	"bufio"
	_ "embed"
	"fmt"
	"hash/crc32"
	"math/bits"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"ds2rb/recomp/arm"
)

//go:embed runtime/recomp.h
var recompHeader []byte

// Functions are grouped into one C file per 64 KB of code, so adding or
// changing a function rewrites only its own file; files whose content did
// not change are not touched (make rebuilds only what changed).
const fileSpan = 0x10000

// Generate writes text_XXXXXXXX.c files with the functions, table.c with
// the address -> function slot table, and recomp.h.
func Generate(p *Program, out string) error {
	if err := os.MkdirAll(out, 0o755); err != nil {
		return err
	}
	written := map[string]bool{}
	put := func(name string, data []byte) error {
		written[name] = true
		path := filepath.Join(out, name)
		if old, err := os.ReadFile(path); err == nil && string(old) == string(data) {
			return nil
		}
		return os.WriteFile(path, data, 0o644)
	}
	if err := put("recomp.h", recompHeader); err != nil {
		return err
	}
	var addrs []uint32
	for a := range p.Funcs {
		addrs = append(addrs, a)
	}
	sort.Slice(addrs, func(i, j int) bool { return addrs[i] < addrs[j] })

	for i := 0; i < len(addrs); {
		span := addrs[i] / fileSpan
		j := i
		for j < len(addrs) && addrs[j]/fileSpan == span {
			j++
		}
		var body strings.Builder
		w := bufio.NewWriter(&body)
		refs := map[uint32]bool{}
		for _, a := range addrs[i:j] {
			g := &gen{p: p, f: p.Funcs[a], w: w, refs: refs}
			g.function()
			fmt.Fprintf(w, "RcFunc p_%08X = f_%08X;\n\n", a, a)
		}
		w.Flush()
		var file strings.Builder
		file.WriteString("#include \"recomp.h\"\n\n")
		own := map[uint32]bool{}
		dyn := map[uint32]bool{}
		for _, a := range addrs[i:j] {
			own[a] = true
			fmt.Fprintf(&file, "void f_%08X(RcCpu* c);\n", a)
			// Entries inside a hand-written block call its dispatcher,
			// which may live in another file.
			if al := p.Funcs[a].AliasOf; al != 0 && !dyn[al] {
				dyn[al] = true
				fmt.Fprintf(&file, "void d_%08X(RcCpu* c, uint32_t start);\n", al)
			}
		}
		var rs []uint32
		for a := range refs {
			rs = append(rs, a)
		}
		sort.Slice(rs, func(x, y int) bool { return rs[x] < rs[y] })
		for _, a := range rs {
			fmt.Fprintf(&file, "extern RcFunc p_%08X;\n", a)
		}
		file.WriteString("\n")
		file.WriteString(body.String())
		if err := put(fmt.Sprintf("text_%08X.c", span*fileSpan), []byte(file.String())); err != nil {
			return err
		}
		i = j
	}

	// rc_addr[i] is the guest address of slot i (sorted, for rc_call's
	// search); *rc_slot[i] the function the code calls there (the runtime
	// may replace it), rc_orig[i] the generated one.
	var t strings.Builder
	t.WriteString("#include \"recomp.h\"\n\n")
	for _, a := range addrs {
		fmt.Fprintf(&t, "void f_%08X(RcCpu* c); extern RcFunc p_%08X;\n", a, a)
	}
	fmt.Fprintf(&t, "\nconst unsigned rc_func_count = %d;\nconst uint32_t rc_addr[] = {\n", len(addrs))
	for _, a := range addrs {
		fmt.Fprintf(&t, "    0x%08X,\n", a)
	}
	t.WriteString("};\nconst RcFunc rc_orig[] = {\n")
	for _, a := range addrs {
		fmt.Fprintf(&t, "    f_%08X,\n", a)
	}
	t.WriteString("};\nRcFunc* const rc_slot[] = {\n")
	for _, a := range addrs {
		fmt.Fprintf(&t, "    &p_%08X,\n", a)
	}
	t.WriteString("};\n")
	// Functions enterable anywhere in their body (computed jumps).
	var dyn []uint32
	for _, a := range addrs {
		if p.Funcs[a].Dynamic {
			dyn = append(dyn, a)
		}
	}
	for _, a := range dyn {
		fmt.Fprintf(&t, "void d_%08X(RcCpu* c, uint32_t start);\n", a)
	}
	fmt.Fprintf(&t, "const unsigned rc_dyn_count = %d;\nconst RcDyn rc_dyn[] = {\n", len(dyn))
	for _, a := range dyn {
		f := p.Funcs[a]
		fmt.Fprintf(&t, "    {0x%08X, 0x%08X, d_%08X},\n", f.Start, f.End, a)
	}
	t.WriteString("    {0, 0, 0}\n};\n")
	// The code.bin this was made from: the runtime refuses another one.
	fmt.Fprintf(&t, "const uint32_t rc_code_crc = 0x%08X;\n", crc32.ChecksumIEEE(p.Im.Data))
	if err := put("table.c", []byte(t.String())); err != nil {
		return err
	}
	// Remove files of an earlier generation that are no longer produced.
	entries, _ := os.ReadDir(out)
	for _, e := range entries {
		n := e.Name()
		if strings.HasSuffix(n, ".c") && !written[n] {
			os.Remove(filepath.Join(out, n))
		}
	}
	return nil
}

type gen struct {
	refs   map[uint32]bool // slots this file calls
	p      *Program
	f      *Func
	w      *bufio.Writer
	labels map[uint32]bool
	order  []uint32
	dyn    bool
}

func (g *gen) out(format string, a ...any) { fmt.Fprintf(g.w, format, a...) }

func r(n int) string { return fmt.Sprintf("c->r[%d]", n) }

var condExpr = [15]string{"c->z", "!c->z", "c->c", "!c->c", "c->n", "!c->n", "c->v", "!c->v",
	"(c->c && !c->z)", "(!c->c || c->z)", "(c->n == c->v)", "(c->n != c->v)",
	"(!c->z && c->n == c->v)", "(c->z || c->n != c->v)", "1"}

// continues reports whether flow can go on to the next instruction.
func (g *gen) continues(in arm.Inst) bool {
	if in.Cond != 14 {
		return true
	}
	switch {
	case in.Kind == arm.Unknown || in.Kind == arm.Bkpt:
		return false
	case in.Kind == arm.Branch && in.Link:
		return true
	case in.Kind == arm.Branch:
		return false
	case in.Kind == arm.BranchReg && in.Link:
		return true
	case in.WritesPC():
		return false
	case in.Kind == arm.Svc && (in.Imm == 0x03 || in.Imm == 0x09):
		return false
	}
	return true
}

func (g *gen) function() {
	f := g.f
	if f.AliasOf != 0 {
		g.out("/* %08X %s: inside the hand-written block at %08X */\nvoid f_%08X(RcCpu* c) { d_%08X(c, 0x%08Xu); }\n\n",
			f.Entry, f.Name, f.AliasOf, f.Entry, f.AliasOf, f.Entry)
		return
	}
	g.labels = map[uint32]bool{}
	for a := range f.Labels {
		g.labels[a] = true
	}
	for a := range f.Insts {
		g.order = append(g.order, a)
	}
	sort.Slice(g.order, func(i, j int) bool { return g.order[i] < g.order[j] })
	// Labels for fall-through that is not to the next emitted instruction.
	for i, a := range g.order {
		in := f.Insts[a]
		nx := a + in.Len
		if g.continues(in) && (i+1 >= len(g.order) || g.order[i+1] != nx) {
			if _, ok := f.Insts[nx]; ok {
				g.labels[nx] = true
			}
		}
	}
	g.dyn = f.Dynamic
	name := f.Name
	if name == "" {
		name = "(entry inside a function)"
	}
	mode := "ARM"
	if f.Thumb {
		mode = "Thumb"
	}
	if g.dyn {
		// Code with computed jumps can be entered anywhere in its body:
		// d_X(c, start) starts at any of its instructions.
		g.out("/* %08X %s, %s, enterable anywhere in %08X-%08X */\nvoid d_%08X(RcCpu* c, uint32_t start) {\n",
			f.Entry, name, mode, f.Start, f.End, f.Entry)
	} else {
		g.out("/* %08X %s, %s */\nvoid f_%08X(RcCpu* c) {\n", f.Entry, name, mode, f.Entry)
	}
	// The return address the function was called with: a computed jump to it
	// (Thumb's pop {r3}; bx r3, or a moved lr) is a return.
	g.out("    const uint32_t lr0 = c->r[14]; (void)lr0;\n")
	g.out("    RC_ENTER(0x%08X);\n", f.Entry)
	if g.dyn {
		g.out("    uint32_t jt = start;\n    goto dispatch;\n")
	} else if len(g.order) == 0 || g.order[0] != f.Entry {
		g.out("    goto L_%08X;\n", f.Entry)
	}
	for i, a := range g.order {
		in := f.Insts[a]
		if g.labels[a] {
			g.out("L_%08X:\n", a)
		}
		g.inst(in)
		nx := a + in.Len
		if g.continues(in) && (i+1 >= len(g.order) || g.order[i+1] != nx) {
			switch {
			case f.Insts[nx].Addr == nx && g.labels[nx]:
				g.out("    goto L_%08X;\n", nx)
			case f.FallInto[nx]:
				g.out("    %s return;\n", g.slotCall(nx))
			default:
				g.out("    rc_unreachable(c, 0x%08X); return;\n", nx)
			}
		}
	}
	if g.dyn {
		g.out("dispatch:\n    switch (jt & ~1u) {\n")
		for _, a := range g.order {
			if g.labels[a] {
				g.out("    case 0x%08X: goto L_%08X;\n", a, a)
			}
		}
		g.out("    default: rc_call(c, jt); return;\n    }\n")
	}
	g.out("}\n\n")
	if g.dyn {
		g.out("void f_%08X(RcCpu* c) { d_%08X(c, 0x%08Xu); }\n\n", f.Entry, f.Entry, f.Entry)
	}
}

// slotCall calls the function at a through its slot; r15 holds the callee's
// address during the call (for overrides that serve several addresses, and
// for backtraces).
func (g *gen) slotCall(a uint32) string {
	if g.refs != nil {
		g.refs[a] = true
	}
	return fmt.Sprintf("c->r[15] = 0x%08Xu; p_%08X(c);", a, a)
}

// call emits a direct call (or tail call) to a.
func (g *gen) call(a uint32) string {
	if _, ok := g.p.Funcs[a]; ok {
		return g.slotCall(a)
	}
	return fmt.Sprintf("rc_call(c, 0x%08X);", a)
}

// ret returns to the caller; the address the code returns to must be the
// one the function was called with (else the runtime reports it: a longjmp
// or a corrupted stack).
func (g *gen) ret(in arm.Inst, expr string) string {
	if g.f.Region {
		// Inside hand-written code a return may be to code of the same
		// block called by hand (add lr, pc, #n; b f).
		return g.jumpTo(expr)
	}
	return fmt.Sprintf("if (((%s) ^ lr0) > 1) rc_badreturn(c, 0x%08X, %s, lr0); RC_LEAVE(0x%08X); return;", expr, in.Addr, expr, in.Addr)
}

// jumpTo emits a jump to a computed address held in expr.
func (g *gen) jumpTo(expr string) string {
	if g.dyn {
		return fmt.Sprintf("{ uint32_t t = %s; if ((t ^ lr0) <= 1) return; jt = t; goto dispatch; }", expr)
	}
	return fmt.Sprintf("{ uint32_t t = %s; if ((t ^ lr0) <= 1) return; rc_call(c, t); return; }", expr)
}

// reg reads register n as an operand of in (r15 reads as the PC value).
func reg(in arm.Inst, n int) string {
	if n == 15 {
		return fmt.Sprintf("0x%08Xu", in.PC())
	}
	return r(n)
}

// op2 returns the C expression of a shifter operand and its carry out
// ("" when the carry flag is unchanged). Register-shifted operands need a
// statement; pre holds it.
func op2(in arm.Inst, o arm.Operand, wantCarry bool) (expr, carry, pre string) {
	if o.Imm {
		if o.Rot != 0 {
			carry = fmt.Sprintf("%du", o.Value>>31)
		}
		return fmt.Sprintf("0x%08Xu", o.Value), carry, ""
	}
	m := reg(in, o.Rm)
	if o.RegShift {
		pre = fmt.Sprintf("uint32_t sc; uint32_t sv = rc_shift_c(c->c, %s, %d, %s, &sc);", m, o.Shift, reg(in, o.Rs))
		return "sv", "sc", pre
	}
	n := o.Amount
	switch o.Shift {
	case arm.LSL:
		if n == 0 {
			return m, "", ""
		}
		return fmt.Sprintf("(%s << %d)", m, n), fmt.Sprintf("((%s >> %d) & 1)", m, 32-n), ""
	case arm.LSR:
		if n == 32 {
			return "0u", fmt.Sprintf("(%s >> 31)", m), ""
		}
		return fmt.Sprintf("(%s >> %d)", m, n), fmt.Sprintf("((%s >> %d) & 1)", m, n-1), ""
	case arm.ASR:
		if n == 32 {
			return fmt.Sprintf("(uint32_t)((int32_t)%s >> 31)", m), fmt.Sprintf("(%s >> 31)", m), ""
		}
		return fmt.Sprintf("(uint32_t)((int32_t)%s >> %d)", m, n), fmt.Sprintf("((%s >> %d) & 1)", m, n-1), ""
	case arm.ROR:
		return fmt.Sprintf("rc_ror(%s, %d)", m, n), fmt.Sprintf("((%s >> %d) & 1)", m, n-1), ""
	default: // RRX
		return fmt.Sprintf("((c->c << 31) | (%s >> 1))", m), fmt.Sprintf("(%s & 1)", m), ""
	}
}

func (g *gen) inst(in arm.Inst) {
	body := g.body(in)
	if in.Cond != 14 && in.Kind != arm.Unknown {
		g.out("    if (%s) { %s }\n", condExpr[in.Cond], body)
	} else {
		g.out("    %s\n", body)
	}
}

func (g *gen) retAddr(in arm.Inst) uint32 {
	a := in.Addr + in.Len
	if in.Thumb {
		a |= 1
	}
	return a
}

func (g *gen) body(in arm.Inst) string {
	switch in.Kind {
	case arm.DataProc:
		return g.dataProc(in)
	case arm.Mem:
		return g.mem(in)
	case arm.Block:
		return g.block(in)
	case arm.Branch:
		return g.branch(in)
	case arm.BranchReg:
		if in.Link {
			return fmt.Sprintf("{ uint32_t t = %s; c->r[14] = 0x%08Xu; rc_call(c, t); }", reg(in, in.Rm), g.retAddr(in))
		}
		if in.Rm == 14 {
			return g.ret(in, "c->r[14]")
		}
		return g.jumpTo(reg(in, in.Rm))
	case arm.Mul:
		return g.mul(in)
	case arm.MulLong:
		return g.mulLong(in)
	case arm.MulHalf:
		return g.mulHalf(in)
	case arm.MulDual:
		return g.mulDual(in)
	case arm.Clz:
		return fmt.Sprintf("%s = rc_clz(%s);", r(in.Rd), reg(in, in.Rm))
	case arm.Sat:
		a, b := reg(in, in.Rm), reg(in, in.Rn)
		switch in.Op {
		case 0:
			return fmt.Sprintf("%s = (uint32_t)rc_sat32(c, (int64_t)(int32_t)%s + (int32_t)%s);", r(in.Rd), a, b)
		case 1:
			return fmt.Sprintf("%s = (uint32_t)rc_sat32(c, (int64_t)(int32_t)%s - (int32_t)%s);", r(in.Rd), a, b)
		case 2:
			return fmt.Sprintf("%s = (uint32_t)rc_sat32(c, (int64_t)(int32_t)%s + rc_sat32(c, 2 * (int64_t)(int32_t)%s));", r(in.Rd), a, b)
		default:
			return fmt.Sprintf("%s = (uint32_t)rc_sat32(c, (int64_t)(int32_t)%s - rc_sat32(c, 2 * (int64_t)(int32_t)%s));", r(in.Rd), a, b)
		}
	case arm.Mrs:
		return fmt.Sprintf("%s = rc_cpsr(c);", r(in.Rd))
	case arm.Msr:
		if in.Op == 1 {
			return "/* msr spsr */"
		}
		v, _, _ := op2(in, in.Op2, false)
		return fmt.Sprintf("rc_set_cpsr(c, %s, %d);", v, in.Mask)
	case arm.Clrex:
		return "c->excl_addr = RC_NO_EXCL;"
	case arm.Hint, arm.Pld, arm.Cps:
		return ";"
	case arm.Svc:
		if in.Imm == 0x03 || in.Imm == 0x09 {
			return fmt.Sprintf("rc_svc(c, 0x%X); return;", in.Imm)
		}
		return fmt.Sprintf("rc_svc(c, 0x%X);", in.Imm)
	case arm.Bkpt:
		return fmt.Sprintf("rc_bkpt(c, 0x%08X, 0x%X); return;", in.Addr, in.Imm)
	case arm.Extend:
		return g.extend(in)
	case arm.Pkh:
		if in.Op == 0 {
			return fmt.Sprintf("%s = (%s & 0xFFFFu) | ((%s << %d) & 0xFFFF0000u);", r(in.Rd), reg(in, in.Rn), reg(in, in.Rm), in.Shift)
		}
		sh := in.Shift
		if sh == 32 {
			sh = 31
		}
		return fmt.Sprintf("%s = (%s & 0xFFFF0000u) | ((uint32_t)((int32_t)%s >> %d) & 0xFFFFu);", r(in.Rd), reg(in, in.Rn), reg(in, in.Rm), sh)
	case arm.Ssat:
		v := reg(in, in.Rm)
		if in.ShiftT == arm.ASR {
			sh := in.Shift
			if sh == 32 {
				sh = 31
			}
			v = fmt.Sprintf("(uint32_t)((int32_t)%s >> %d)", v, sh)
		} else if in.Shift != 0 {
			v = fmt.Sprintf("(%s << %d)", v, in.Shift)
		}
		if in.Op == 0 {
			return fmt.Sprintf("%s = rc_ssat(c, (int32_t)%s, %d);", r(in.Rd), v, in.Imm+1)
		}
		return fmt.Sprintf("%s = rc_usat(c, (int32_t)%s, %d);", r(in.Rd), v, in.Imm)
	case arm.Sat16:
		f := "rc_ssat"
		n := in.Imm + 1
		if in.Op == 1 {
			f, n = "rc_usat", in.Imm
		}
		m := reg(in, in.Rm)
		return fmt.Sprintf("%s = (%s(c, (int16_t)%s, %d) & 0xFFFFu) | (%s(c, (int16_t)(%s >> 16), %d) << 16);", r(in.Rd), f, m, n, f, m, n)
	case arm.Rev:
		m := reg(in, in.Rm)
		switch in.Op {
		case 0:
			return fmt.Sprintf("%s = __builtin_bswap32(%s);", r(in.Rd), m)
		case 1:
			return fmt.Sprintf("{ uint32_t t = %s; %s = ((t & 0x00FF00FFu) << 8) | ((t >> 8) & 0x00FF00FFu); }", m, r(in.Rd))
		default:
			return fmt.Sprintf("%s = (uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t)%s);", r(in.Rd), m)
		}
	case arm.Sel:
		return fmt.Sprintf("%s = rc_sel(c, %s, %s);", r(in.Rd), reg(in, in.Rn), reg(in, in.Rm))
	case arm.Parallel:
		return fmt.Sprintf("%s = rc_parallel(c, %d, %s, %s);", r(in.Rd), in.Op, reg(in, in.Rn), reg(in, in.Rm))
	case arm.MemEx:
		return g.memEx(in)
	case arm.Swp:
		ld, st := "ld32", "st32"
		if in.Size == 1 {
			ld, st = "ld8", "st8"
		}
		return fmt.Sprintf("{ uint32_t a = %s, t = %s(a); %s(a, %s); %s = t; }", reg(in, in.Rn), ld, st, reg(in, in.Rm), r(in.Rd))
	case arm.CoprocReg:
		if in.Cp != 15 {
			return fmt.Sprintf("rc_trap(c, 0x%08X, 0x%08X); return;", in.Addr, in.Raw)
		}
		if in.Load {
			if in.CRn == 13 && in.CRm == 0 && in.Opc2 == 3 {
				return fmt.Sprintf("%s = c->tls;", r(in.Rd))
			}
			return fmt.Sprintf("%s = rc_cp15_read(c, 0x%08X, 0x%08X);", r(in.Rd), in.Addr, in.Raw)
		}
		return fmt.Sprintf("rc_cp15_write(c, 0x%08X, 0x%08X, %s);", in.Addr, in.Raw, reg(in, in.Rd))
	case arm.VfpArith:
		return g.vfpArith(in)
	case arm.VfpCmp:
		if in.Dbl {
			if in.Zero {
				return fmt.Sprintf("rc_fcmp(c, c->f.d[%d], 0.0);", in.Vd)
			}
			return fmt.Sprintf("rc_fcmp(c, c->f.d[%d], c->f.d[%d]);", in.Vd, in.Vm)
		}
		if in.Zero {
			return fmt.Sprintf("rc_fcmp(c, c->f.s[%d], 0.0);", in.Vd)
		}
		return fmt.Sprintf("rc_fcmp(c, c->f.s[%d], c->f.s[%d]);", in.Vd, in.Vm)
	case arm.VfpCvt:
		return g.vfpCvt(in)
	case arm.VfpMovCore:
		if in.Load {
			return fmt.Sprintf("%s = c->f.w[%d];", r(in.Rd), in.Vn)
		}
		return fmt.Sprintf("c->f.w[%d] = %s;", in.Vn, reg(in, in.Rd))
	case arm.VfpMovHalf:
		k := 2 * in.Vn
		if in.X {
			k++
		}
		if in.Load {
			return fmt.Sprintf("%s = c->f.w[%d];", r(in.Rd), k)
		}
		return fmt.Sprintf("c->f.w[%d] = %s;", k, reg(in, in.Rd))
	case arm.VfpMov2:
		a, b := in.Vm, in.Vm+1
		if in.Dbl {
			a, b = 2*in.Vm, 2*in.Vm+1
		}
		if in.Load {
			return fmt.Sprintf("%s = c->f.w[%d]; %s = c->f.w[%d];", r(in.Rd), a, r(in.Rn), b)
		}
		return fmt.Sprintf("c->f.w[%d] = %s; c->f.w[%d] = %s;", a, reg(in, in.Rd), b, reg(in, in.Rn))
	case arm.VfpSys:
		if in.Load {
			switch {
			case in.Reg == 1 && in.Rd == 15:
				return "rc_fpscr_to_flags(c);"
			case in.Reg == 1:
				return fmt.Sprintf("%s = c->fpscr;", r(in.Rd))
			case in.Reg == 8:
				return fmt.Sprintf("%s = 0x40000000u;", r(in.Rd)) // FPEXC: enabled
			default:
				return fmt.Sprintf("%s = 0x410120B4u;", r(in.Rd)) // FPSID (VFP11)
			}
		}
		if in.Reg == 1 {
			return fmt.Sprintf("c->fpscr = %s;", reg(in, in.Rd))
		}
		return ";"
	case arm.VfpMem:
		return g.vfpMem(in)
	case arm.VfpBlock:
		return g.vfpBlock(in)
	}
	return fmt.Sprintf("rc_trap(c, 0x%08X, 0x%08X); return;", in.Addr, in.Raw)
}

func (g *gen) dataProc(in arm.Inst) string {
	logical := in.Op == arm.AND || in.Op == arm.EOR || in.Op == arm.TST || in.Op == arm.TEQ ||
		in.Op == arm.ORR || in.Op == arm.MOV || in.Op == arm.BIC || in.Op == arm.MVN
	b, carry, pre := op2(in, in.Op2, in.S && logical)
	a := ""
	if in.Op != arm.MOV && in.Op != arm.MVN {
		a = reg(in, in.Rn)
	}
	var e string
	switch in.Op {
	case arm.AND, arm.TST:
		e = fmt.Sprintf("%s & %s", a, b)
	case arm.EOR, arm.TEQ:
		e = fmt.Sprintf("%s ^ %s", a, b)
	case arm.ORR:
		e = fmt.Sprintf("%s | %s", a, b)
	case arm.BIC:
		e = fmt.Sprintf("%s & ~%s", a, b)
	case arm.MOV:
		e = b
	case arm.MVN:
		e = "~" + b
	case arm.ADD, arm.CMN:
		if in.S {
			e = fmt.Sprintf("rc_add(c, %s, %s, 0)", a, b)
		} else {
			e = fmt.Sprintf("%s + %s", a, b)
		}
	case arm.SUB, arm.CMP:
		if in.S {
			e = fmt.Sprintf("rc_sub(c, %s, %s, 1)", a, b)
		} else {
			e = fmt.Sprintf("%s - %s", a, b)
		}
	case arm.RSB:
		if in.S {
			e = fmt.Sprintf("rc_sub(c, %s, %s, 1)", b, a)
		} else {
			e = fmt.Sprintf("%s - %s", b, a)
		}
	case arm.ADC:
		if in.S {
			e = fmt.Sprintf("rc_add(c, %s, %s, c->c)", a, b)
		} else {
			e = fmt.Sprintf("%s + %s + c->c", a, b)
		}
	case arm.SBC:
		if in.S {
			e = fmt.Sprintf("rc_sub(c, %s, %s, c->c)", a, b)
		} else {
			e = fmt.Sprintf("%s - %s - !c->c", a, b)
		}
	case arm.RSC:
		if in.S {
			e = fmt.Sprintf("rc_sub(c, %s, %s, c->c)", b, a)
		} else {
			e = fmt.Sprintf("%s - %s - !c->c", b, a)
		}
	}
	var s strings.Builder
	s.WriteString("{ ")
	if pre != "" {
		s.WriteString(pre + " ")
	}
	s.WriteString("uint32_t res = " + e + "; ")
	if in.S && logical {
		s.WriteString("rc_nz(c, res); ")
		if carry != "" {
			s.WriteString("c->c = " + carry + "; ")
		}
	}
	test := in.Op >= arm.TST && in.Op <= arm.CMN
	if !test {
		if in.Rd == 15 {
			if in.IsReturn() {
				s.WriteString(g.ret(in, "res") + " ")
			} else {
				s.WriteString(g.jumpTo("res") + " ")
			}
		} else {
			s.WriteString(r(in.Rd) + " = res; ")
		}
	} else {
		s.WriteString("(void)res; ")
	}
	s.WriteString("}")
	// Jump tables: add pc, pc, rX, lsl #2.
	if t, ok := g.f.Tables[in.Addr]; ok {
		return g.table(in, t, reg(in, in.Op2.Rm))
	}
	return s.String()
}

func (g *gen) table(in arm.Inst, t []uint32, idx string) string {
	var s strings.Builder
	fmt.Fprintf(&s, "switch (%s) {", idx)
	for k, a := range t {
		if g.f.Insts[a].Addr == a && a >= g.f.Start && a < g.f.End {
			g.labels[a] = true
			fmt.Fprintf(&s, " case %d: goto L_%08X;", k, a)
		} else {
			fmt.Fprintf(&s, " case %d: %s return;", k, g.call(a))
		}
	}
	fmt.Fprintf(&s, " default: rc_unreachable(c, 0x%08X); return; }", in.Addr)
	return s.String()
}

// offset expression of a load/store.
func offExpr(in arm.Inst) string {
	o := in.Off
	if o.Imm {
		return fmt.Sprintf("0x%Xu", o.Value)
	}
	e, _, _ := op2(in, arm.Operand{Rm: o.Rm, Shift: o.Shift, Amount: o.Amount}, false)
	return e
}

func (g *gen) mem(in arm.Inst) string {
	if t, ok := g.f.Tables[in.Addr]; ok {
		return g.table(in, t, r(in.Off.Rm))
	}
	// Literal from read-only memory: a constant.
	if in.Load && in.Rn == 15 && in.Off.Imm && in.P && !in.W && in.Rd != 15 {
		a := in.PC() - in.Off.Value
		if in.U {
			a = in.PC() + in.Off.Value
		}
		if g.p.Im.ReadOnly(a) && g.p.Im.ReadOnly(a+uint32(in.Size)-1) {
			w, _ := g.p.Im.Word(a &^ 3)
			w2, _ := g.p.Im.Word((a &^ 3) + 4)
			v := uint64(w) | uint64(w2)<<32
			v >>= 8 * (a & 3)
			var x uint32
			switch {
			case in.Size == 4:
				x = uint32(v)
			case in.Size == 2 && in.Signed:
				x = uint32(int32(int16(v)))
			case in.Size == 2:
				x = uint32(uint16(v))
			case in.Size == 1 && in.Signed:
				x = uint32(int32(int8(v)))
			case in.Size == 1:
				x = uint32(uint8(v))
			default:
				goto general
			}
			return fmt.Sprintf("%s = 0x%08Xu;", r(in.Rd), x)
		}
	}
	if t, ok := g.p.constJump(in); ok {
		return fmt.Sprintf("%s return;", g.call(t&^1))
	}
general:
	base := reg(in, in.Rn)
	off := offExpr(in)
	sign := "+"
	if !in.U {
		sign = "-"
	}
	var s strings.Builder
	s.WriteString("{ ")
	addr := fmt.Sprintf("%s %s %s", base, sign, off)
	if in.P {
		fmt.Fprintf(&s, "uint32_t a = %s; ", addr)
	} else {
		fmt.Fprintf(&s, "uint32_t a = %s; ", base)
	}
	wb := ""
	if in.W || !in.P {
		if in.P {
			wb = fmt.Sprintf("%s = a; ", r(in.Rn))
		} else {
			wb = fmt.Sprintf("%s = a %s %s; ", r(in.Rn), sign, off)
		}
	}
	if in.Load {
		switch in.Size {
		case 8:
			fmt.Fprintf(&s, "uint32_t v0 = ld32(a), v1 = ld32(a + 4); %s%s = v0; %s = v1; ", wb, r(in.Rd), r(in.Rd+1))
		default:
			fn := map[int]string{4: "ld32", 2: "ld16", 1: "ld8"}[in.Size]
			if in.Signed {
				fn = map[int]string{2: "lds16", 1: "lds8"}[in.Size]
			}
			fmt.Fprintf(&s, "uint32_t v = %s(a); %s", fn, wb)
			if in.Rd == 15 {
				if in.IsReturn() {
					s.WriteString(g.ret(in, "v") + " ")
				} else {
					s.WriteString(g.jumpTo("v") + " ")
				}
			} else {
				fmt.Fprintf(&s, "%s = v; ", r(in.Rd))
			}
		}
	} else {
		switch in.Size {
		case 8:
			fmt.Fprintf(&s, "st32(a, %s); st32(a + 4, %s); %s", reg(in, in.Rd), reg(in, in.Rd+1), wb)
		default:
			fn := map[int]string{4: "st32", 2: "st16", 1: "st8"}[in.Size]
			v := reg(in, in.Rd)
			if in.Rd == 15 {
				v = fmt.Sprintf("0x%08Xu", in.Addr+12)
			}
			fmt.Fprintf(&s, "%s(a, %s); %s", fn, v, wb)
		}
	}
	s.WriteString("}")
	return s.String()
}

func (g *gen) memEx(in arm.Inst) string {
	a := reg(in, in.Rn)
	if in.Load {
		switch in.Size {
		case 8:
			return fmt.Sprintf("rc_ldrexd(c, %s, &%s, &%s);", a, r(in.Rd), r(in.Rd+1))
		case 4:
			return fmt.Sprintf("%s = rc_ldrex32(c, %s);", r(in.Rd), a)
		case 2:
			return fmt.Sprintf("%s = rc_ldrex16(c, %s);", r(in.Rd), a)
		default:
			return fmt.Sprintf("%s = rc_ldrex8(c, %s);", r(in.Rd), a)
		}
	}
	// STREX Rd, Rm, [Rn]: a compare-and-swap against what LDREX read.
	switch in.Size {
	case 8:
		return fmt.Sprintf("%s = rc_strexd(c, %s, %s, %s);", r(in.Rd), a, reg(in, in.Rm), reg(in, in.Rm+1))
	case 4:
		return fmt.Sprintf("%s = rc_strex32(c, %s, %s);", r(in.Rd), a, reg(in, in.Rm))
	case 2:
		return fmt.Sprintf("%s = rc_strex16(c, %s, %s);", r(in.Rd), a, reg(in, in.Rm))
	default:
		return fmt.Sprintf("%s = rc_strex8(c, %s, %s);", r(in.Rd), a, reg(in, in.Rm))
	}
}

func (g *gen) block(in arm.Inst) string {
	n := uint32(bits.OnesCount16(in.RegList))
	var start string
	base := reg(in, in.Rn)
	switch {
	case !in.P && in.U: // IA
		start = base
	case in.P && in.U: // IB
		start = base + " + 4"
	case !in.P && !in.U: // DA
		start = fmt.Sprintf("%s - %d", base, 4*n-4)
	default: // DB
		start = fmt.Sprintf("%s - %d", base, 4*n)
	}
	var s strings.Builder
	fmt.Fprintf(&s, "{ uint32_t a = %s; ", start)
	wbExpr := ""
	if in.W {
		if in.U {
			wbExpr = fmt.Sprintf("%s = %s + %d; ", r(in.Rn), base, 4*n)
		} else {
			wbExpr = fmt.Sprintf("%s = %s - %d; ", r(in.Rn), base, 4*n)
		}
	}
	k := 0
	if in.Load {
		// Load into temporaries first: the base may be in the list.
		for i := 0; i < 16; i++ {
			if in.RegList>>i&1 != 0 {
				fmt.Fprintf(&s, "uint32_t t%d = ld32(a + %d); ", i, 4*k)
				k++
			}
		}
		if in.W && in.RegList>>uint(in.Rn)&1 == 0 {
			s.WriteString(wbExpr)
		}
		for i := 0; i < 15; i++ {
			if in.RegList>>i&1 != 0 {
				fmt.Fprintf(&s, "%s = t%d; ", r(i), i)
			}
		}
		if in.RegList&0x8000 != 0 {
			if in.IsReturn() {
				s.WriteString(g.ret(in, "t15") + " ")
			} else {
				s.WriteString(g.jumpTo("t15") + " ")
			}
		}
	} else {
		for i := 0; i < 16; i++ {
			if in.RegList>>i&1 != 0 {
				v := r(i)
				if i == 15 {
					v = fmt.Sprintf("0x%08Xu", in.Addr+12)
				}
				fmt.Fprintf(&s, "st32(a + %d, %s); ", 4*k, v)
				k++
			}
		}
		s.WriteString(wbExpr)
	}
	s.WriteString("}")
	return s.String()
}

func (g *gen) branch(in arm.Inst) string {
	if in.Link {
		return fmt.Sprintf("c->r[14] = 0x%08Xu; ", g.retAddr(in)) + g.call(in.Target)
	}
	if in.Target >= g.f.Start && in.Target < g.f.End {
		if _, ok := g.f.Insts[in.Target]; ok {
			if in.Target <= in.Addr {
				// A loop: let a higher-priority thread that became ready run.
				return fmt.Sprintf("RC_POLL(); goto L_%08X;", in.Target)
			}
			return fmt.Sprintf("goto L_%08X;", in.Target)
		}
	}
	return g.call(in.Target) + " return;"
}

func (g *gen) mul(in arm.Inst) string {
	e := fmt.Sprintf("%s * %s", reg(in, in.Rm), reg(in, in.Rs))
	switch in.Op {
	case 1:
		e = fmt.Sprintf("%s * %s + %s", reg(in, in.Rm), reg(in, in.Rs), reg(in, in.Rn))
	case 2:
		e = fmt.Sprintf("%s - %s * %s", reg(in, in.Rn), reg(in, in.Rm), reg(in, in.Rs))
	}
	if in.S {
		return fmt.Sprintf("{ uint32_t res = %s; rc_nz(c, res); %s = res; }", e, r(in.Rd))
	}
	return fmt.Sprintf("%s = %s;", r(in.Rd), e)
}

func (g *gen) mulLong(in arm.Inst) string {
	hi, lo := in.Rd, in.Rn
	m, s := reg(in, in.Rm), reg(in, in.Rs)
	var e string
	switch in.Op {
	case 0:
		e = fmt.Sprintf("(uint64_t)%s * %s", m, s)
	case 1:
		e = fmt.Sprintf("(uint64_t)%s * %s + ((uint64_t)%s << 32 | %s)", m, s, r(hi), r(lo))
	case 2:
		e = fmt.Sprintf("(uint64_t)((int64_t)(int32_t)%s * (int32_t)%s)", m, s)
	case 3:
		e = fmt.Sprintf("(uint64_t)((int64_t)(int32_t)%s * (int32_t)%s) + ((uint64_t)%s << 32 | %s)", m, s, r(hi), r(lo))
	default: // UMAAL
		e = fmt.Sprintf("(uint64_t)%s * %s + %s + %s", m, s, r(hi), r(lo))
	}
	flags := ""
	if in.S {
		flags = "c->n = (uint32_t)(x >> 63); c->z = x == 0; "
	}
	return fmt.Sprintf("{ uint64_t x = %s; %s%s = (uint32_t)x; %s = (uint32_t)(x >> 32); }", e, flags, r(lo), r(hi))
}

func half(v string, top bool) string {
	if top {
		return fmt.Sprintf("(int32_t)(int16_t)(%s >> 16)", v)
	}
	return fmt.Sprintf("(int32_t)(int16_t)%s", v)
}

func (g *gen) mulHalf(in arm.Inst) string {
	m, s := reg(in, in.Rm), reg(in, in.Rs)
	switch in.Op {
	case 0: // SMLAxy
		// Wraps; Q records the overflow.
		return fmt.Sprintf("{ int64_t x = (int64_t)(%s * %s) + (int32_t)%s; if (x != (int32_t)x) c->q = 1; %s = (uint32_t)x; }", half(m, in.X), half(s, in.Y), reg(in, in.Rn), r(in.Rd))
	case 3: // SMULxy
		return fmt.Sprintf("%s = (uint32_t)(%s * %s);", r(in.Rd), half(m, in.X), half(s, in.Y))
	case 1: // SMLAWy
		return fmt.Sprintf("{ int64_t x = (((int64_t)(int32_t)%s * %s) >> 16) + (int32_t)%s; if (x != (int32_t)x) c->q = 1; %s = (uint32_t)x; }", m, half(s, in.Y), reg(in, in.Rn), r(in.Rd))
	case 4: // SMULWy
		return fmt.Sprintf("%s = (uint32_t)(((int64_t)(int32_t)%s * %s) >> 16);", r(in.Rd), m, half(s, in.Y))
	default: // SMLALxy: RdHi = Rd, RdLo = Rn
		return fmt.Sprintf("{ int64_t x = (int64_t)((uint64_t)%s << 32 | %s) + (int64_t)(%s * %s); %s = (uint32_t)x; %s = (uint32_t)((uint64_t)x >> 32); }",
			r(in.Rd), r(in.Rn), half(m, in.X), half(s, in.Y), r(in.Rn), r(in.Rd))
	}
}

func (g *gen) mulDual(in arm.Inst) string {
	n, m := reg(in, in.Rm), reg(in, in.Rs)
	if in.X {
		m = fmt.Sprintf("rc_ror(%s, 16)", m)
	}
	p1 := fmt.Sprintf("((int64_t)%s * %s)", half(n, false), half(m, false))
	p2 := fmt.Sprintf("((int64_t)%s * %s)", half(n, true), half(m, true))
	acc := in.Rn != 15
	switch in.Op {
	case 0, 1:
		op := "+"
		if in.Op == 1 {
			op = "-"
		}
		e := fmt.Sprintf("%s %s %s", p1, op, p2)
		if acc {
			return fmt.Sprintf("{ int64_t x = %s + (int32_t)%s; if (x != (int32_t)x) c->q = 1; %s = (uint32_t)x; }", e, r(in.Rn), r(in.Rd))
		}
		return fmt.Sprintf("{ int64_t x = %s; if (x != (int32_t)x) c->q = 1; %s = (uint32_t)x; }", e, r(in.Rd))
	case 2, 3: // SMLALD / SMLSLD: RdHi = Rd, RdLo = Rn
		op := "+"
		if in.Op == 3 {
			op = "-"
		}
		return fmt.Sprintf("{ int64_t x = (int64_t)((uint64_t)%s << 32 | %s) + %s %s %s; %s = (uint32_t)x; %s = (uint32_t)((uint64_t)x >> 32); }",
			r(in.Rd), r(in.Rn), p1, op, p2, r(in.Rn), r(in.Rd))
	case 4, 5: // SMMLA / SMMUL / SMMLS (R = bit 5 rounds)
		round := ""
		if in.Raw>>5&1 != 0 {
			round = " + 0x80000000LL"
		}
		prod := fmt.Sprintf("(int64_t)(int32_t)%s * (int32_t)%s", reg(in, in.Rm), reg(in, in.Rs))
		if in.Op == 4 {
			if acc {
				return fmt.Sprintf("%s = (uint32_t)((((int64_t)%s << 32) + %s%s) >> 32);", r(in.Rd), r(in.Rn), prod, round)
			}
			return fmt.Sprintf("%s = (uint32_t)((%s%s) >> 32);", r(in.Rd), prod, round)
		}
		return fmt.Sprintf("%s = (uint32_t)((((int64_t)%s << 32) - %s%s) >> 32);", r(in.Rd), r(in.Rn), prod, round)
	default: // USAD8 / USADA8
		var s strings.Builder
		s.WriteString("{ uint32_t x = 0, a = " + reg(in, in.Rm) + ", b = " + reg(in, in.Rs) + "; for (int i = 0; i < 32; i += 8) { int d = (int)((a >> i) & 255) - (int)((b >> i) & 255); x += (uint32_t)(d < 0 ? -d : d); } ")
		if acc {
			s.WriteString("x += " + r(in.Rn) + "; ")
		}
		s.WriteString(r(in.Rd) + " = x; }")
		return s.String()
	}
}

func (g *gen) extend(in arm.Inst) string {
	m := reg(in, in.Rm)
	if in.Rot != 0 {
		m = fmt.Sprintf("rc_ror(%s, %d)", m, in.Rot)
	}
	var e string
	switch in.Op {
	case 0: // SXTB16
		e = fmt.Sprintf("(((uint32_t)(int32_t)(int8_t)%s & 0xFFFFu) | ((uint32_t)(int32_t)(int8_t)(%s >> 16) << 16))", m, m)
	case 2:
		e = fmt.Sprintf("(uint32_t)(int32_t)(int8_t)%s", m)
	case 3:
		e = fmt.Sprintf("(uint32_t)(int32_t)(int16_t)%s", m)
	case 4: // UXTB16
		e = fmt.Sprintf("(%s & 0x00FF00FFu)", m)
	case 6:
		e = fmt.Sprintf("(%s & 0xFFu)", m)
	default:
		e = fmt.Sprintf("(%s & 0xFFFFu)", m)
	}
	if in.Rn != 15 {
		if in.Op == 0 || in.Op == 4 { // 16-bit lanes add separately
			return fmt.Sprintf("{ uint32_t x = %s, y = %s; %s = ((y + x) & 0xFFFFu) | (((y >> 16) + (x >> 16)) << 16); }", e, r(in.Rn), r(in.Rd))
		}
		e = fmt.Sprintf("%s + %s", r(in.Rn), e)
	}
	return fmt.Sprintf("%s = %s;", r(in.Rd), e)
}

func (g *gen) vfpArith(in arm.Inst) string {
	R := func(i int) string {
		if in.Dbl {
			return fmt.Sprintf("c->f.d[%d]", i)
		}
		return fmt.Sprintf("c->f.s[%d]", i)
	}
	d, n, m := R(in.Vd), R(in.Vn), R(in.Vm)
	sq := "sqrtf"
	ab := "fabsf"
	if in.Dbl {
		sq, ab = "sqrt", "fabs"
	}
	switch in.Op {
	case arm.VMLA:
		return fmt.Sprintf("%s = %s + %s * %s;", d, d, n, m)
	case arm.VMLS:
		return fmt.Sprintf("%s = %s - %s * %s;", d, d, n, m)
	case arm.VNMLS:
		return fmt.Sprintf("%s = -%s + %s * %s;", d, d, n, m)
	case arm.VNMLA:
		return fmt.Sprintf("%s = -%s - %s * %s;", d, d, n, m)
	case arm.VMUL:
		return fmt.Sprintf("%s = %s * %s;", d, n, m)
	case arm.VNMUL:
		return fmt.Sprintf("%s = -(%s * %s);", d, n, m)
	case arm.VADD:
		return fmt.Sprintf("%s = %s + %s;", d, n, m)
	case arm.VSUB:
		return fmt.Sprintf("%s = %s - %s;", d, n, m)
	case arm.VDIV:
		return fmt.Sprintf("%s = %s / %s;", d, n, m)
	case arm.VMOV:
		if in.Dbl {
			return fmt.Sprintf("c->f.w[%d] = c->f.w[%d]; c->f.w[%d] = c->f.w[%d];", 2*in.Vd, 2*in.Vm, 2*in.Vd+1, 2*in.Vm+1)
		}
		return fmt.Sprintf("c->f.w[%d] = c->f.w[%d];", in.Vd, in.Vm)
	case arm.VABS:
		return fmt.Sprintf("%s = %s(%s);", d, ab, m)
	case arm.VNEG:
		if in.Dbl {
			return fmt.Sprintf("c->f.w[%d] = c->f.w[%d]; c->f.w[%d] = c->f.w[%d] ^ 0x80000000u;", 2*in.Vd, 2*in.Vm, 2*in.Vd+1, 2*in.Vm+1)
		}
		return fmt.Sprintf("c->f.w[%d] = c->f.w[%d] ^ 0x80000000u;", in.Vd, in.Vm)
	default:
		return fmt.Sprintf("%s = %s(%s);", d, sq, m)
	}
}

func (g *gen) vfpCvt(in arm.Inst) string {
	S := func(i int) string { return fmt.Sprintf("c->f.s[%d]", i) }
	D := func(i int) string { return fmt.Sprintf("c->f.d[%d]", i) }
	W := func(i int) string { return fmt.Sprintf("c->f.w[%d]", i) }
	rnd := func(x string) string {
		if in.E {
			return fmt.Sprintf("rc_round_mode(c, %s)", x)
		}
		return x
	}
	switch in.Op {
	case arm.CvtF64F32:
		return fmt.Sprintf("%s = (double)%s;", D(in.Vd), S(in.Vm))
	case arm.CvtF32F64:
		return fmt.Sprintf("%s = (float)%s;", S(in.Vd), D(in.Vm))
	case arm.CvtF32U32:
		return fmt.Sprintf("%s = (float)%s;", S(in.Vd), W(in.Vm))
	case arm.CvtF32S32:
		return fmt.Sprintf("%s = (float)(int32_t)%s;", S(in.Vd), W(in.Vm))
	case arm.CvtF64U32:
		return fmt.Sprintf("%s = (double)%s;", D(in.Vd), W(in.Vm))
	case arm.CvtF64S32:
		return fmt.Sprintf("%s = (double)(int32_t)%s;", D(in.Vd), W(in.Vm))
	case arm.CvtU32F32:
		return fmt.Sprintf("%s = rc_f2u(%s);", W(in.Vd), rnd("(double)"+S(in.Vm)))
	case arm.CvtS32F32:
		return fmt.Sprintf("%s = rc_f2s(%s);", W(in.Vd), rnd("(double)"+S(in.Vm)))
	case arm.CvtU32F64:
		return fmt.Sprintf("%s = rc_f2u(%s);", W(in.Vd), rnd(D(in.Vm)))
	default:
		return fmt.Sprintf("%s = rc_f2s(%s);", W(in.Vd), rnd(D(in.Vm)))
	}
}

func (g *gen) vfpMem(in arm.Inst) string {
	base := reg(in, in.Rn)
	if in.Rn == 15 {
		base = fmt.Sprintf("0x%08Xu", in.PC()&^3)
	}
	sign := "+"
	if !in.U {
		sign = "-"
	}
	a := fmt.Sprintf("%s %s %d", base, sign, in.Imm)
	if in.Dbl {
		if in.Load {
			return fmt.Sprintf("{ uint32_t a = %s; c->f.w[%d] = ld32(a); c->f.w[%d] = ld32(a + 4); }", a, 2*in.Vd, 2*in.Vd+1)
		}
		return fmt.Sprintf("{ uint32_t a = %s; st32(a, c->f.w[%d]); st32(a + 4, c->f.w[%d]); }", a, 2*in.Vd, 2*in.Vd+1)
	}
	if in.Load {
		return fmt.Sprintf("c->f.w[%d] = ld32(%s);", in.Vd, a)
	}
	return fmt.Sprintf("st32(%s, c->f.w[%d]);", a, in.Vd)
}

func (g *gen) vfpBlock(in arm.Inst) string {
	words := in.Count
	first := in.Vd
	if in.Dbl {
		words = in.Count * 2
		first = in.Vd * 2
	}
	bytes := 4 * words
	if in.Dbl && in.Op == 1 { // FLDMX/FSTMX: one extra word
		bytes += 4
	}
	base := reg(in, in.Rn)
	var s strings.Builder
	if in.P { // decrement before
		fmt.Fprintf(&s, "{ uint32_t a = %s - %d; ", base, bytes)
	} else {
		fmt.Fprintf(&s, "{ uint32_t a = %s; ", base)
	}
	for k := 0; k < words; k++ {
		if in.Load {
			fmt.Fprintf(&s, "c->f.w[%d] = ld32(a + %d); ", first+k, 4*k)
		} else {
			fmt.Fprintf(&s, "st32(a + %d, c->f.w[%d]); ", 4*k, first+k)
		}
	}
	if in.W {
		if in.U {
			fmt.Fprintf(&s, "%s = %s + %d; ", r(in.Rn), base, bytes)
		} else {
			fmt.Fprintf(&s, "%s = %s - %d; ", r(in.Rn), base, bytes)
		}
	}
	s.WriteString("}")
	return s.String()
}
