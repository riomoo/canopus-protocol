package main

import "core:fmt"
import "core:os"
import "core:slice"
import "core:strings"

import "arm"

// generate_insn_tests writes a C file with one function per distinct
// instruction encoding the program uses (those that do not change the flow),
// for the instruction-level comparison against the emulator
// (tools/emu/test_recomp_insns.py). Instructions that read memory come with
// their kind so the test can point base registers at scratch memory.
generate_insn_tests :: proc(p: ^Program, path: string) -> string {
	Key :: struct {
		raw:   u32,
		thumb: bool,
	}
	seen: map[Key]arm.Inst
	for _, f in p.funcs {
		for a, in_ in f.insts {
			if f.swept[a] || !testable(p, in_) {
				continue
			}
			k := Key{in_.raw, in_.thumb}
			if k not_in seen {
				seen[k] = in_
			}
		}
	}
	list: [dynamic]arm.Inst
	for _, in_ in seen {
		append(&list, in_)
	}
	slice.sort_by(list[:], proc(a, b: arm.Inst) -> bool {
		if a.addr != b.addr {
			return a.addr < b.addr
		}
		if a.raw != b.raw {
			return a.raw < b.raw
		}
		return !a.thumb && b.thumb
	})
	w := strings.builder_make()
	strings.write_string(&w, "#include \"recomp.h\"\n\n")
	dummy := new(Func)
	for in_, i in list {
		g := Gen{p = p, f = dummy, w = &w}
		fmt.sbprintf(&w, "static void t_%d(RcCpu* c) {{\n", i)
		inst(&g, in_)
		strings.write_string(&w, "}\n")
		free_all(context.temp_allocator)
	}
	strings.write_string(&w, "\ntypedef struct { uint32_t addr, raw, len, thumb, mem, rn, rm; void (*fn)(RcCpu*); } RcTest;\n")
	fmt.sbprintf(&w, "const unsigned rc_test_count = %d;\nconst RcTest rc_tests[] = {{\n", len(list))
	for in_, i in list {
		mem := 1 if touches_memory(in_) else 0
		th := 1 if in_.thumb else 0
		rn, rm := 255, 255
		if mem == 1 {
			rn = in_.rn
			if in_.kind == .Mem && !in_.off.imm {
				rm = in_.off.rm
			}
		}
		fmt.sbprintf(&w, "    {{0x%08X, 0x%08X, %d, %d, %d, %d, %d, t_%d}},\n", in_.addr, in_.raw, in_.len, th, mem, rn, rm, i)
	}
	strings.write_string(&w, "};\n")
	if err := os.write_entire_file(path, strings.to_string(w)); err != nil {
		return fmt.aprintf("write %s: %v", path, err)
	}
	return ""
}

touches_memory :: proc(in_: arm.Inst) -> bool {
	#partial switch in_.kind {
	case .Mem, .Block, .MemEx, .Swp, .VfpMem, .VfpBlock:
		return true
	}
	return false
}

// testable: instructions whose whole effect is on registers, flags and
// memory (no flow change, no kernel or coprocessor call, no literal turned
// into a constant from the image the test does not load the same way).
testable :: proc(p: ^Program, in_: arm.Inst) -> bool {
	#partial switch in_.kind {
	case .Unknown, .Branch, .BranchReg, .Svc, .Bkpt, .CoprocReg, .Hint, .Pld, .Cps, .Clrex, .Msr, .Mrs, .VfpSys:
		return false
	}
	if arm.writes_pc(in_) {
		return false
	}
	if in_.kind == .MemEx && !in_.load {
		return false // STREX: the emulator has no exclusive monitor set up; the recompiler always succeeds
	}
	#partial switch in_.kind {
	case .Mem, .VfpMem:
		if in_.rn == 15 || in_.rn == in_.rd && (in_.w || !in_.p) {
			return false
		}
		if in_.kind == .Mem && !in_.off.imm && (in_.off.rm == in_.rn || in_.off.rm == 15) {
			return false
		}
		if in_.kind == .Mem && in_.size == 8 && (in_.rd == 14 || in_.rd & 1 != 0) {
			return false
		}
	case .Block:
		if in_.reg_list & (1 << 15) != 0 || in_.rn == 15 || in_.reg_list == 0 {
			return false
		}
		if !in_.load && (in_.reg_list >> uint(in_.rn)) & 1 != 0 && in_.w {
			return false // stores a base it also writes back: unpredictable
		}
	case .DataProc:
		if in_.rn == 15 || in_.op2.rm == 15 && !in_.op2.imm || in_.op2.reg_shift && in_.op2.rs == 15 {
			return false
		}
	}
	return true
}
