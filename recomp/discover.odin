package main

import "core:fmt"
import "core:os"
import "core:slice"
import "core:strings"

import "arm"

// Image is code.bin as loaded at 0x100000, with its segment layout from the
// exheader.
Image :: struct {
	data:                   []byte,
	text_start, text_end:   u32,
	ro_start, ro_end:       u32,
	data_start, data_end:   u32,
	bss_end:                u32,
	stack_size:             u32,
}

BASE :: 0x100000

le32 :: #force_inline proc(b: []byte) -> u32 {
	return u32(b[0]) | u32(b[1]) << 8 | u32(b[2]) << 16 | u32(b[3]) << 24
}

load_image :: proc(code, exheader: string) -> (im: ^Image, err: string) {
	d, e1 := os.read_entire_file(code, context.allocator)
	if e1 != nil {
		return nil, fmt.aprintf("open %s: %v", code, e1)
	}
	h, e2 := os.read_entire_file(exheader, context.allocator)
	if e2 != nil {
		return nil, fmt.aprintf("open %s: %v", exheader, e2)
	}
	if len(h) < 0x40 {
		return nil, fmt.aprintf("%s: too short for an exheader", exheader)
	}
	u :: proc(h: []byte, o: int) -> u32 { return le32(h[o:]) }
	im = new(Image)
	im.data = d
	im.text_start, im.text_end = u(h, 0x10), u(h, 0x10) + u(h, 0x18)
	im.ro_start, im.ro_end = u(h, 0x20), u(h, 0x20) + u(h, 0x28)
	im.data_start, im.data_end = u(h, 0x30), u(h, 0x30) + u(h, 0x38)
	im.bss_end = im.data_end + u(h, 0x3C)
	im.stack_size = u(h, 0x1C)
	return im, ""
}

in_text :: #force_inline proc(im: ^Image, a: u32) -> bool { return a >= im.text_start && a < im.text_end }

word :: proc(im: ^Image, a: u32) -> (u32, bool) {
	o := i64(a) - BASE
	if o < 0 || o + 4 > i64(len(im.data)) {
		return 0, false
	}
	return le32(im.data[o:]), true
}

half :: proc(im: ^Image, a: u32) -> u16 {
	o := i64(a) - BASE
	if o < 0 || o + 2 > i64(len(im.data)) {
		return 0
	}
	return u16(im.data[o]) | u16(im.data[o + 1]) << 8
}

// read_only reports whether a is in a segment the game cannot write (text,
// rodata): literals there are constants.
read_only :: proc(im: ^Image, a: u32) -> bool {
	return (a >= im.text_start && a < im.text_end) || (a >= im.ro_start && a < im.ro_end)
}

image_inst :: proc(im: ^Image, a: u32, thumb: bool) -> arm.Inst {
	if thumb {
		return arm.decode_thumb(a, half(im, a), half(im, a + 2))
	}
	w, _ := word(im, a)
	return arm.decode(a, w)
}

// Func is one function to generate.
Func :: struct {
	entry:     u32,
	thumb:     bool,
	start:     u32, // range the body may use (Ghidra's bounds)
	end:       u32,
	name:      string,
	insts:     map[u32]arm.Inst,
	swept:     map[u32]bool,  // decoded by sweeping, not reached by flow
	labels:    map[u32]bool,
	tables:    map[u32][]u32, // jump table at instruction -> targets
	calls:     map[u32]bool,  // call / tail-call targets -> Thumb
	fall_into: map[u32]bool,  // flow runs past the body into another function: a tail call
	computed:  bool,          // has a computed jump inside the body
	region:    bool,          // a hand-written block: one function, enterable anywhere
	alias_of:  u32,           // an entry inside a region: calls the region's function
	returns:   bool,
	problems:  [dynamic]string,
}

free_func :: proc(f: ^Func) {
	delete(f.insts)
	delete(f.swept)
	delete(f.labels)
	for _, t in f.tables {
		delete(t)
	}
	delete(f.tables)
	delete(f.calls)
	delete(f.fall_into)
	for s in f.problems {
		delete(s)
	}
	delete(f.problems)
	free(f)
}

Program :: struct {
	im:           ^Image,
	funcs:        map[u32]^Func,
	names:        map[u32]string,
	bounds:       map[u32][2]u32, // Ghidra function start -> [start, end)
	thumb:        map[u32]bool,   // entries in Thumb
	starts:       [dynamic]u32,
	no_return:    map[u32]bool,
	pointers:     map[u32]bool, // text addresses found as pointers (literals, data); odd = Thumb
	extra:        map[u32]bool, // entries added by hand (hints file)
	decoded_set:  map[u32]bool,
	decoded_ok:   bool,
	data_pointer: map[u32]bool,
	hint_thumb:   map[u32]bool, // Thumb entries from the hints
	arm_called:   map[u32]bool, // targets of ARM-mode calls
	literal:      map[u32]bool, // words loaded as literals (pools)
	code_pointer: map[u32]bool, // text addresses code loads as literals
	regions:      [dynamic][2]u32, // hand-written blocks (hints)
}

new_program :: proc(im: ^Image) -> ^Program {
	p := new(Program)
	p.im = im
	return p
}

// parse_uint parses all of s as an unsigned number of the given base that
// fits in bits bits.
parse_uint :: proc(s: string, base: u64, nbits: uint) -> (u64, bool) {
	if len(s) == 0 {
		return 0, false
	}
	v: u64
	for c in transmute([]byte)s {
		d: u64
		switch c {
		case '0' ..= '9':
			d = u64(c - '0')
		case 'a' ..= 'f':
			d = u64(c - 'a' + 10)
		case 'A' ..= 'F':
			d = u64(c - 'A' + 10)
		case:
			return 0, false
		}
		if d >= base {
			return 0, false
		}
		v = v * base + d
		if v >> nbits != 0 {
			return 0, false
		}
	}
	return v, true
}

// split_lines splits text into lines, dropping a trailing \r.
split_lines :: proc(text: string) -> []string {
	lines := strings.split(text, "\n")
	for &l in lines {
		l = strings.trim_suffix(l, "\r")
	}
	if len(lines) > 0 && lines[len(lines) - 1] == "" {
		lines = lines[:len(lines) - 1]
	}
	return lines
}

// load_func_table reads a CSV with at least addr (hex), name, size columns.
load_func_table :: proc(p: ^Program, path: string) -> string {
	data, err := os.read_entire_file(path, context.allocator)
	if err != nil {
		return fmt.aprintf("open %s: %v", path, err)
	}
	lines := split_lines(string(data))
	first := true
	col: map[string]int
	defer delete(col)
	for line in lines {
		if line == "" {
			continue
		}
		rec := strings.split(line, ",")
		defer delete(rec)
		if first {
			for h, i in rec {
				col[h] = i
			}
			first = false
			continue
		}
		get :: proc(rec: []string, col: map[string]int, name: string) -> string {
			i := col[name] or_else 0
			return rec[i] if i < len(rec) else ""
		}
		a, _ := parse_uint(get(rec, col, "addr"), 16, 32)
		sz, _ := parse_uint(get(rec, col, "size"), 10, 32)
		if !in_text(p.im, u32(a)) {
			continue
		}
		p.bounds[u32(a)] = [2]u32{u32(a), u32(a + sz)}
		p.names[u32(a)] = strings.clone(get(rec, col, "name"))
	}
	clear(&p.starts)
	for a in p.bounds {
		append(&p.starts, a)
	}
	slice.sort(p.starts[:])
	return ""
}

// load_hints reads lines "thumb <hex addr>" (a Thumb function), "entry <hex
// addr>" (an extra entry point), "reltable <start> <end>" and "region
// <start> <end>"; # starts a comment.
load_hints :: proc(p: ^Program, path: string) -> string {
	data, err := os.read_entire_file(path, context.allocator)
	if err != nil {
		return fmt.aprintf("open %s: %v", path, err)
	}
	for raw_line in split_lines(string(data)) {
		line := raw_line
		if i := strings.index_byte(line, '#'); i >= 0 {
			line = line[:i]
		}
		line = strings.trim_space(line)
		fs := strings.fields_proc(line, proc(r: rune) -> bool { return r == ' ' || r == ',' || r == '\t' })
		if len(fs) < 2 {
			continue
		}
		bad := fmt.aprintf("%s: %q", path, line)
		a64, ok := parse_uint(strings.trim_prefix(fs[1], "0x"), 16, 32)
		if !ok {
			return bad
		}
		a := u32(a64)
		switch fs[0] {
		case "reltable": // reltable <start> <end>: words that are offsets from their own address to a function
			if len(fs) < 3 {
				return bad
			}
			e, ok2 := parse_uint(strings.trim_prefix(fs[2], "0x"), 16, 32)
			if !ok2 {
				return bad
			}
			for x := a; x + 4 <= u32(e); x += 4 {
				w, okw := word(p.im, x)
				if !okw {
					break
				}
				t := x + w
				if t & 1 != 0 {
					p.thumb[t &~ 1] = true
					p.hint_thumb[t &~ 1] = true
				}
				p.extra[t &~ 1] = true
			}
		case "region": // region <start> <end>: hand-written code, one function enterable anywhere
			if len(fs) < 3 {
				return bad
			}
			e, ok2 := parse_uint(strings.trim_prefix(fs[2], "0x"), 16, 32)
			if !ok2 {
				return bad
			}
			append(&p.regions, [2]u32{a, u32(e)})
			p.extra[a] = true
		case "thumb":
			p.thumb[a] = true
			p.hint_thumb[a] = true
		case "entry":
			p.extra[a] = true
		}
	}
	return ""
}

// bounds_for returns the body range for an entry: Ghidra's bounds of the
// function containing it (an entry in the middle of a function may use the
// rest of that function).
bounds_for :: proc(p: ^Program, a: u32) -> (u32, u32) {
	// first index whose start is above a, minus one
	lo, hi := 0, len(p.starts)
	for lo < hi {
		m := int(uint(lo + hi) >> 1)
		if p.starts[m] > a {
			hi = m
		} else {
			lo = m + 1
		}
	}
	i := lo - 1
	if i >= 0 {
		b := p.bounds[p.starts[i]]
		if a >= b[0] && a < b[1] {
			return b[0], b[1]
		}
	}
	end := p.im.text_end
	if i + 1 < len(p.starts) {
		end = p.starts[i + 1]
	}
	return a, end
}

writes_reg :: proc(in_: arm.Inst, r: int) -> bool {
	#partial switch in_.kind {
	case .DataProc:
		return in_.rd == r && (in_.op < arm.TST || in_.op > arm.CMN)
	case .Mem:
		return in_.load && in_.rd == r || in_.w && in_.rn == r
	case .Mul, .Clz, .Extend:
		return in_.rd == r
	}
	return false
}

// jump_table recognises ARM switches:
//
//	cmp rX, #n ; [bhi/bcs default] ; ldr{ls,cc} pc, [pc, rX, lsl #2] ; .word ...
//	cmp rX, #n ; addls pc, pc, rX, lsl #2 ; b default ; b case0 ...
jump_table :: proc(p: ^Program, in_: arm.Inst) -> ([]u32, bool) {
	if in_.thumb {
		return nil, false
	}
	idx: int
	is_ldr := in_.kind == .Mem && in_.load && in_.size == 4 && in_.rn == 15 && in_.p && in_.u && !in_.w &&
		!in_.off.imm && in_.off.shift == arm.LSL && in_.off.amount == 2
	is_add := in_.kind == .DataProc && in_.op == arm.ADD && in_.rn == 15 && !in_.op2.imm && !in_.op2.reg_shift &&
		in_.op2.shift == arm.LSL && in_.op2.amount == 2
	switch {
	case is_ldr:
		idx = in_.off.rm
	case is_add:
		idx = in_.op2.rm
	case:
		return nil, false
	}
	guard := in_.cond
	n: u32
	found := false
	for k := u32(1); k <= 8; k += 1 {
		q := image_inst(p.im, in_.addr - 4 * k, false)
		if q.kind == .DataProc && q.op == arm.CMP && q.rn == idx && q.op2.imm {
			n = q.op2.value
			found = true
			break
		}
		if q.kind == .Branch && !q.link && q.cond != 14 && guard == 14 {
			guard = q.cond ~ 1 // the path that reaches the jump
			continue
		}
		if writes_reg(q, idx) || arm.writes_pc(q) {
			return nil, false
		}
	}
	if !found {
		return nil, false
	}
	switch guard {
	case 9: // LS
		n += 1
	case 3: // CC
	case:
		return nil, false
	}
	if n == 0 || n > 4096 {
		return nil, false
	}
	t := make([dynamic]u32, 0, n)
	for k := u32(0); k < n; k += 1 {
		if is_add {
			append(&t, in_.addr + 8 + k * 4)
			continue
		}
		w, ok := word(p.im, in_.addr + 8 + k * 4)
		if !ok || !in_text(p.im, w) {
			delete(t)
			return nil, false
		}
		append(&t, w)
	}
	return t[:], true
}

region_of :: proc(p: ^Program, a: u32) -> ([2]u32, bool) {
	for r in p.regions {
		if a >= r[0] && a < r[1] {
			return r, true
		}
	}
	return {}, false
}

// walk decodes a function's body from its entry.
walk :: proc(p: ^Program, entry: u32) -> ^Func {
	if r, ok := region_of(p, entry); ok {
		if entry != r[0] {
			f := new(Func)
			f^ = Func{entry = entry, start = r[0], end = r[1], alias_of = r[0], name = p.names[entry], returns = true}
			f.calls[r[0]] = false
			return f
		}
		return walk_region(p, r)
	}
	thumb := p.thumb[entry]
	start, end := bounds_for(p, entry)
	f := new(Func)
	f^ = Func{entry = entry, thumb = thumb, start = start, end = end, name = p.names[entry]}
	f.labels[entry] = true
	in_body :: #force_inline proc(a, start, end: u32) -> bool { return a >= start && a < end }
	todo: [dynamic]u32
	defer delete(todo)
	append(&todo, entry)
	for len(todo) > 0 {
		pc := pop(&todo)
		for {
			if pc in f.insts {
				break
			}
			if !in_body(pc, start, end) && pc != entry {
				// Ghidra ended the function early (a call it took for
				// no-return, or shared tail code): carry on into the next
				// function as a tail call.
				f.fall_into[pc] = true
				f.calls[pc] = p.thumb[pc] || thumb
				if p.thumb[pc] != thumb {
					append(&f.problems, fmt.aprintf("falls into a function of the other mode at %08X", pc))
				}
				if !p.no_return[pc] {
					f.returns = true
				}
				break
			}
			in_ := image_inst(p.im, pc, thumb)
			f.insts[pc] = in_
			if in_.kind == .Unknown {
				append(&f.problems, fmt.aprintf("unknown instruction %08X at %08X", in_.raw, pc))
				break
			}
			note_pointers(p, in_)
			cont := true
			terminal := in_.cond == 14 // a conditional branch or return falls through
			switch {
			case in_.kind == .Branch && in_.link:
				// Decoding goes on after every call: a callee wrongly taken
				// for one that never returns must not cut its caller short.
				f.calls[in_.target] = thumb != in_.exchange
			case in_.kind == .Branch:
				if in_body(in_.target, start, end) {
					f.labels[in_.target] = true
					append(&todo, in_.target)
				} else {
					f.calls[in_.target] = thumb // tail call
					if !p.no_return[in_.target] {
						f.returns = true
					}
				}
				cont = !terminal
			case arm.is_return(in_):
				f.returns = true
				cont = !terminal
			case arm.writes_pc(in_):
				if t, ok := jump_table(p, in_); ok {
					f.tables[pc] = t
					for a in t {
						if in_body(a, start, end) {
							f.labels[a] = true
							append(&todo, a)
						} else {
							f.calls[a] = false
							f.returns = true
						}
					}
					// PC reads 8 ahead: the table starts two words on, and the
					// word between is the default path.
					cont = !terminal
				} else if t2, ok2 := const_jump(p, in_); ok2 {
					f.calls[t2 &~ 1] = t2 & 1 != 0 // ldr pc, =target: a long tail call
					if t2 & 1 != 0 {
						mark_thumb(p, t2 &~ 1)
					}
					f.returns = true
					cont = !terminal
				} else if in_.kind == .BranchReg && in_.link {
					// indirect call
				} else if in_.kind == .BranchReg {
					f.returns = true // indirect tail call
					cont = !terminal
				} else {
					f.computed = true // computed jump: anywhere in the body, or a tail call
					f.returns = true
					cont = !terminal
				}
			case in_.kind == .Svc && (in_.imm == 0x03 || in_.imm == 0x09):
				cont = !terminal // ExitProcess / ExitThread
			}
			if !cont {
				break
			}
			pc += in_.len
		}
	}
	if f.computed {
		// Every instruction of the body may be a target: decode all of it.
		step := u32(2) if thumb else u32(4)
		for a := start; a < end; a += step {
			if a not_in f.insts {
				f.insts[a] = image_inst(p.im, a, thumb)
				f.swept[a] = true
			}
			f.labels[a] = true
		}
	}
	return f
}

// const_jump: ldr pc, [pc, #imm] from read-only memory.
const_jump :: proc(p: ^Program, in_: arm.Inst) -> (u32, bool) {
	if in_.kind != .Mem || !in_.load || in_.rd != 15 || in_.rn != 15 || !in_.off.imm || !in_.p || in_.w || in_.size != 4 {
		return 0, false
	}
	la := arm.pc(in_) - in_.off.value
	if in_.u {
		la = arm.pc(in_) + in_.off.value
	}
	if !read_only(p.im, la) {
		return 0, false
	}
	w, ok := word(p.im, la)
	return w, ok && in_text(p.im, w &~ 1)
}

// walk_region decodes a whole hand-written block as one function: every word
// is an instruction that may be a target (data words in it decode as junk
// that never runs).
walk_region :: proc(p: ^Program, r: [2]u32) -> ^Func {
	f := new(Func)
	f^ = Func{entry = r[0], start = r[0], end = r[1], name = fmt.aprintf("hand-written block %08X-%08X", r[0], r[1]),
		computed = true, region = true, returns = true}
	for a := r[0]; a < r[1]; a += 4 {
		in_ := image_inst(p.im, a, false)
		f.insts[a] = in_
		f.labels[a] = true
		note_pointers(p, in_)
		if in_.kind == .Branch && (in_.target < r[0] || in_.target >= r[1]) && in_text(p.im, in_.target) {
			f.calls[in_.target] = in_.exchange
		}
	}
	return f
}

note_pointers :: proc(p: ^Program, in_: arm.Inst) {
	if in_.kind == .VfpMem && in_.rn == 15 {
		la := arm.pc(in_) &~ 3 - in_.imm
		if in_.u {
			la = arm.pc(in_) &~ 3 + in_.imm
		}
		p.literal[la] = true
		if in_.dbl {
			p.literal[la + 4] = true
		}
	}
	if in_.kind == .Mem && in_.load && in_.rn == 15 && in_.off.imm {
		la := arm.pc(in_) - in_.off.value
		if in_.u {
			la = arm.pc(in_) + in_.off.value
		}
		p.literal[la &~ 3] = true
	}
	if in_.kind == .Mem && in_.load && in_.rn == 15 && in_.off.imm && in_.rd != 15 && in_.size == 4 {
		la := arm.pc(in_) - in_.off.value
		if in_.u {
			la = arm.pc(in_) + in_.off.value
		}
		if w, ok := word(p.im, la); ok && in_text(p.im, w) {
			p.pointers[w] = true
			p.code_pointer[w] = true
		}
	}
	if in_.kind == .DataProc && in_.rn == 15 && in_.op2.imm && (in_.op == arm.ADD || in_.op == arm.SUB) && in_.rd != 15 {
		a := arm.pc(in_) + in_.op2.value
		if in_.op == arm.SUB {
			a = arm.pc(in_) - in_.op2.value
		}
		if in_text(p.im, a) {
			p.pointers[a] = true
		}
	}
}

// walk_queue walks every entry on the queue and the functions they call.
walk_queue :: proc(p: ^Program, queue: ^[dynamic]u32) {
	for len(queue) > 0 {
		a := pop(queue)
		if a in p.funcs || !in_text(p.im, a) {
			continue
		}
		f := walk(p, a)
		p.funcs[a] = f
		for c, th in f.calls {
			if th {
				mark_thumb(p, c)
			} else {
				p.arm_called[c] = true
				if !p.hint_thumb[c] {
					delete_key(&p.thumb, c)
				}
			}
			append(queue, c)
		}
	}
}

// discover walks every function reachable from the entry point, the function
// table, the hints and pointers, until no new entry turns up; then marks
// functions that never return and walks again so calls to them end paths.
discover :: proc(p: ^Program) {
	seeds: [dynamic]u32
	append(&seeds, p.im.text_start)
	for a in p.bounds {
		append(&seeds, a)
	}
	for a in p.extra {
		append(&seeds, a)
	}
	for a := p.im.ro_start; a + 4 <= p.im.data_end; a += 4 {
		if w, ok := word(p.im, a); ok && in_text(p.im, w) {
			p.pointers[w] = true
			p.data_pointer[w] = true
		}
	}
	queue: [dynamic]u32
	for round := 0; round < 4; round += 1 {
		for _, f in p.funcs {
			free_func(f)
		}
		clear(&p.funcs)
		clear(&queue)
		append(&queue, ..seeds[:])
		walk_queue(p, &queue)
		// Pointers become entries when they land on a known function start
		// or on an instruction already decoded in the same mode.
		clear(&p.decoded_set)
		p.decoded_ok = false
		for w in p.pointers {
			a, th := w &~ 1, w & 1 != 0
			if a in p.funcs {
				continue
			}
			// A pointer into Thumb code is odd, into ARM code 4-aligned;
			// anything else is a data address.
			cs, ce := bounds_for(p, a)
			in_known := cs in p.bounds
			in_known = in_known && a >= cs && a < ce
			if !th && a & 3 != 0 {
				continue
			}
			if in_known && th != p.thumb[cs] {
				continue
			}
			if th && !in_known && looks_like_start(p, a, true) {
				p.thumb[a] = true // Thumb code Ghidra did not find
				append(&queue, a)
				continue
			}
			// Outside every known function (code Ghidra missed): any address
			// that decodes as an instruction.
			// Inside one, only what looks like a function start (Ghidra
			// sometimes merges a function into the one before it).
			plausible := looks_like_start(p, a, false) ||
				!in_known && p.code_pointer[w] && image_inst(p.im, a, false).kind != .Unknown
			if a in p.bounds || decoded(p, a) || plausible {
				if th {
					p.thumb[a] = true
				}
				append(&queue, a)
			}
		}
		walk_queue(p, &queue)
		// Gap sweep: code no decoded function covers that starts like a
		// function (push {..., lr}) is a function reached only through a
		// computed address.
		covered: map[u32]bool
		for _, f in p.funcs {
			for a, in_ in f.insts {
				for k := u32(0); k < in_.len; k += 2 {
					covered[a + k] = true
				}
			}
		}
		for a := p.im.text_start; a + 4 <= p.im.text_end; a += 2 {
			if covered[a] || p.literal[a &~ 3] {
				continue
			}
			if a in p.funcs {
				continue
			}
			w, _ := word(p.im, a)
			if a & 3 == 0 && w & 0xFFFFC000 == 0xE92D4000 && !p.hint_thumb[a] {
				append(&queue, a)
			} else if h := half(p.im, a); h & 0xFF00 == 0xB500 && !p.arm_called[a] {
				if known := a in p.bounds; !known || p.hint_thumb[a] {
					p.thumb[a] = true
					append(&queue, a)
				}
			}
		}
		delete(covered)
		walk_queue(p, &queue)
		changed := false
		for a, f in p.funcs {
			nr := !f.returns
			if nr != p.no_return[a] {
				p.no_return[a] = nr
				changed = true
			}
		}
		if !changed {
			break
		}
	}
	delete(queue)
	delete(seeds)
}

// looks_like_start: a function prologue (push, sub sp, mov ip sp, bx lr), or
// code right after a return or an unconditional branch.
looks_like_start :: proc(p: ^Program, a: u32, thumb: bool) -> bool {
	if thumb {
		h := half(p.im, a)
		if h & 0xFF00 == 0xB500 || h & 0xFE00 == 0xB400 || h == 0x4770 || h & 0xFF80 == 0xB080 {
			return true
		}
		if p.literal[(a - 2) &~ 3] && a & 3 == 0 {
			return true
		}
		prev := half(p.im, a - 2)
		return prev == 0x4770 || prev & 0xFF00 == 0xBD00 || prev & 0xF800 == 0xE000
	}
	w, _ := word(p.im, a)
	if w & 0xFFFF0000 == 0xE92D0000 || w == 0xE12FFF1E || w & 0xFFFFF000 == 0xE24DD000 || w == 0xE1A0C00D || w & 0xFFFF0FFF == 0xE52D0004 {
		return true
	}
	if p.literal[a - 4] {
		return true // after a literal pool
	}
	prev := image_inst(p.im, a - 4, false)
	if prev.kind == .Unknown || prev.cond != 14 {
		return false
	}
	return arm.writes_pc(prev) && !(prev.kind == .Branch && prev.link) && !(prev.kind == .BranchReg && prev.link)
}

// mark_thumb records a Thumb entry unless the address is known ARM: a
// function in Ghidra's table that is not a Thumb one, or a target of ARM
// calls (code mis-decoded as Thumb would otherwise turn ARM functions into
// Thumb ones).
mark_thumb :: proc(p: ^Program, a: u32) {
	if p.hint_thumb[a] {
		p.thumb[a] = true
		return
	}
	if a in p.bounds || p.arm_called[a] {
		return
	}
	p.thumb[a] = true
}

decoded :: proc(p: ^Program, a: u32) -> bool {
	if !p.decoded_ok {
		p.decoded_ok = true
		for _, f in p.funcs {
			for x in f.insts {
				if !f.swept[x] {
					p.decoded_set[x] = true
				}
			}
		}
	}
	return p.decoded_set[a]
}
