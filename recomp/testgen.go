package main

import (
	"bufio"
	"fmt"
	"os"
	"sort"

	"ds2rb/recomp/arm"
)

// GenerateInsnTests writes a C file with one function per distinct
// instruction encoding the program uses (those that do not change the flow),
// for the instruction-level comparison against the emulator
// (tools/emu/test_recomp_insns.py). Instructions that read memory come with
// their kind so the test can point base registers at scratch memory.
func GenerateInsnTests(p *Program, path string) error {
	type key struct {
		raw   uint32
		thumb bool
	}
	seen := map[key]arm.Inst{}
	for _, f := range p.Funcs {
		for a, in := range f.Insts {
			if f.Swept[a] || !testable(p, in) {
				continue
			}
			k := key{in.Raw, in.Thumb}
			if _, ok := seen[k]; !ok {
				seen[k] = in
			}
		}
	}
	var list []arm.Inst
	for _, in := range seen {
		list = append(list, in)
	}
	sort.Slice(list, func(i, j int) bool { return list[i].Addr < list[j].Addr })
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	w := bufio.NewWriterSize(f, 1<<20)
	w.WriteString("#include \"recomp.h\"\n\n")
	dummy := &Func{Insts: map[uint32]arm.Inst{}, Tables: map[uint32][]uint32{}}
	for i, in := range list {
		g := &gen{p: p, f: dummy, w: w}
		fmt.Fprintf(w, "static void t_%d(RcCpu* c) {\n", i)
		g.inst(in)
		w.WriteString("}\n")
	}
	w.WriteString("\ntypedef struct { uint32_t addr, raw, len, thumb, mem, rn, rm; void (*fn)(RcCpu*); } RcTest;\n")
	fmt.Fprintf(w, "const unsigned rc_test_count = %d;\nconst RcTest rc_tests[] = {\n", len(list))
	for i, in := range list {
		mem := 0
		if touchesMemory(in) {
			mem = 1
		}
		th := 0
		if in.Thumb {
			th = 1
		}
		rn, rm := 255, 255
		if mem == 1 {
			rn = in.Rn
			if in.Kind == arm.Mem && !in.Off.Imm {
				rm = in.Off.Rm
			}
		}
		fmt.Fprintf(w, "    {0x%08X, 0x%08X, %d, %d, %d, %d, %d, t_%d},\n", in.Addr, in.Raw, in.Len, th, mem, rn, rm, i)
	}
	w.WriteString("};\n")
	return w.Flush()
}

func touchesMemory(in arm.Inst) bool {
	switch in.Kind {
	case arm.Mem, arm.Block, arm.MemEx, arm.Swp, arm.VfpMem, arm.VfpBlock:
		return true
	}
	return false
}

// testable: instructions whose whole effect is on registers, flags and
// memory (no flow change, no kernel or coprocessor call, no literal turned
// into a constant from the image the test does not load the same way).
func testable(p *Program, in arm.Inst) bool {
	switch in.Kind {
	case arm.Unknown, arm.Branch, arm.BranchReg, arm.Svc, arm.Bkpt, arm.CoprocReg, arm.Hint, arm.Pld, arm.Cps,
		arm.Clrex, arm.Msr, arm.Mrs, arm.VfpSys:
		return false
	}
	if in.WritesPC() {
		return false
	}
	if in.Kind == arm.MemEx && !in.Load {
		return false // STREX: the emulator has no exclusive monitor set up; the recompiler always succeeds
	}
	switch in.Kind {
	case arm.Mem, arm.VfpMem:
		if in.Rn == 15 || in.Rn == in.Rd && (in.W || !in.P) {
			return false
		}
		if in.Kind == arm.Mem && !in.Off.Imm && (in.Off.Rm == in.Rn || in.Off.Rm == 15) {
			return false
		}
		if in.Kind == arm.Mem && in.Size == 8 && (in.Rd == 14 || in.Rd&1 != 0) {
			return false
		}
	case arm.Block:
		if in.RegList&(1<<15) != 0 || in.Rn == 15 || in.RegList == 0 {
			return false
		}
		if !in.Load && in.RegList>>uint(in.Rn)&1 != 0 && in.W {
			return false // stores a base it also writes back: unpredictable
		}
	case arm.DataProc:
		if in.Rn == 15 || in.Op2.Rm == 15 && !in.Op2.Imm || in.Op2.RegShift && in.Op2.Rs == 15 {
			return false
		}
	}
	return true
}
