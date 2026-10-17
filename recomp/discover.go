package main

import (
	"bufio"
	"encoding/binary"
	"encoding/csv"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"

	"ds2rb/recomp/arm"
)

// Image is code.bin as loaded at 0x100000, with its segment layout from the
// exheader.
type Image struct {
	Data               []byte
	TextStart, TextEnd uint32
	RoStart, RoEnd     uint32
	DataStart, DataEnd uint32
	BssEnd             uint32
	StackSize          uint32
}

const Base = 0x100000

func LoadImage(code, exheader string) (*Image, error) {
	d, err := os.ReadFile(code)
	if err != nil {
		return nil, err
	}
	h, err := os.ReadFile(exheader)
	if err != nil {
		return nil, err
	}
	u := func(o int) uint32 { return binary.LittleEndian.Uint32(h[o:]) }
	im := &Image{Data: d}
	im.TextStart, im.TextEnd = u(0x10), u(0x10)+u(0x18)
	im.RoStart, im.RoEnd = u(0x20), u(0x20)+u(0x28)
	im.DataStart, im.DataEnd = u(0x30), u(0x30)+u(0x38)
	im.BssEnd = im.DataEnd + u(0x3C)
	im.StackSize = u(0x1C)
	return im, nil
}

func (im *Image) InText(a uint32) bool { return a >= im.TextStart && a < im.TextEnd }

func (im *Image) Word(a uint32) (uint32, bool) {
	o := int64(a) - Base
	if o < 0 || o+4 > int64(len(im.Data)) {
		return 0, false
	}
	return binary.LittleEndian.Uint32(im.Data[o:]), true
}

func (im *Image) Half(a uint32) uint16 {
	o := int64(a) - Base
	if o < 0 || o+2 > int64(len(im.Data)) {
		return 0
	}
	return binary.LittleEndian.Uint16(im.Data[o:])
}

// ReadOnly reports whether a is in a segment the game cannot write (text,
// rodata): literals there are constants.
func (im *Image) ReadOnly(a uint32) bool {
	return (a >= im.TextStart && a < im.TextEnd) || (a >= im.RoStart && a < im.RoEnd)
}

func (im *Image) Inst(a uint32, thumb bool) arm.Inst {
	if thumb {
		return arm.DecodeThumb(a, im.Half(a), im.Half(a+2))
	}
	w, _ := im.Word(a)
	return arm.Decode(a, w)
}

// Func is one function to generate.
type Func struct {
	Entry    uint32
	Thumb    bool
	Start    uint32 // range the body may use (Ghidra's bounds)
	End      uint32
	Name     string
	Insts    map[uint32]arm.Inst
	Swept    map[uint32]bool // decoded by sweeping, not reached by flow
	Labels   map[uint32]bool
	Tables   map[uint32][]uint32 // jump table at instruction -> targets
	Calls    map[uint32]bool     // call / tail-call targets -> Thumb
	FallInto map[uint32]bool     // flow runs past the body into another function: a tail call
	Dynamic  bool                // has a computed jump inside the body
	Region   bool                // a hand-written block: one function, enterable anywhere
	AliasOf  uint32              // an entry inside a region: calls the region's function
	Returns  bool
	Problems []string
}

type Program struct {
	Im          *Image
	Funcs       map[uint32]*Func
	Names       map[uint32]string
	Bounds      map[uint32][2]uint32 // Ghidra function start -> [start, end)
	Thumb       map[uint32]bool      // entries in Thumb
	starts      []uint32
	NoReturn    map[uint32]bool
	Pointers    map[uint32]bool // text addresses found as pointers (literals, data); odd = Thumb
	Extra       map[uint32]bool // entries added by hand (hints file)
	decodedSet  map[uint32]bool
	dataPointer map[uint32]bool
	hintThumb   map[uint32]bool // Thumb entries from the hints
	armCalled   map[uint32]bool // targets of ARM-mode calls
	literal     map[uint32]bool // words loaded as literals (pools)
	codePointer map[uint32]bool // text addresses code loads as literals
	Regions     [][2]uint32     // hand-written blocks (hints)
}

func NewProgram(im *Image) *Program {
	return &Program{Im: im, Funcs: map[uint32]*Func{}, Names: map[uint32]string{}, Bounds: map[uint32][2]uint32{},
		Thumb: map[uint32]bool{}, NoReturn: map[uint32]bool{}, Pointers: map[uint32]bool{}, Extra: map[uint32]bool{},
		hintThumb: map[uint32]bool{}, armCalled: map[uint32]bool{}, literal: map[uint32]bool{},
		codePointer: map[uint32]bool{}}
}

// LoadFuncTable reads a CSV with at least addr (hex), name, size columns.
func LoadFuncTable(p *Program, path string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	r := csv.NewReader(f)
	r.FieldsPerRecord = -1
	hdr, err := r.Read()
	if err != nil {
		return err
	}
	col := map[string]int{}
	for i, h := range hdr {
		col[h] = i
	}
	for {
		rec, err := r.Read()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		a, _ := strconv.ParseUint(rec[col["addr"]], 16, 32)
		sz, _ := strconv.ParseUint(rec[col["size"]], 10, 32)
		if !p.Im.InText(uint32(a)) {
			continue
		}
		p.Bounds[uint32(a)] = [2]uint32{uint32(a), uint32(a + sz)}
		p.Names[uint32(a)] = rec[col["name"]]
	}
	p.starts = p.starts[:0]
	for a := range p.Bounds {
		p.starts = append(p.starts, a)
	}
	sort.Slice(p.starts, func(i, j int) bool { return p.starts[i] < p.starts[j] })
	return nil
}

// LoadHints reads lines "thumb <hex addr>" (a Thumb function) and
// "entry <hex addr>" (an extra entry point); # starts a comment.
func LoadHints(p *Program, path string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(strings.SplitN(sc.Text(), "#", 2)[0])
		fs := strings.FieldsFunc(line, func(r rune) bool { return r == ' ' || r == ',' || r == '\t' })
		if len(fs) < 2 {
			continue
		}
		a, err := strconv.ParseUint(strings.TrimPrefix(fs[1], "0x"), 16, 32)
		if err != nil {
			return fmt.Errorf("%s: %q", path, line)
		}
		switch fs[0] {
		case "reltable": // reltable <start> <end>: words that are offsets from their own address to a function
			if len(fs) < 3 {
				return fmt.Errorf("%s: %q", path, line)
			}
			e, err := strconv.ParseUint(strings.TrimPrefix(fs[2], "0x"), 16, 32)
			if err != nil {
				return fmt.Errorf("%s: %q", path, line)
			}
			for x := uint32(a); x+4 <= uint32(e); x += 4 {
				w, ok := p.Im.Word(x)
				if !ok {
					break
				}
				t := x + w
				if t&1 != 0 {
					p.Thumb[t&^1] = true
					p.hintThumb[t&^1] = true
				}
				p.Extra[t&^1] = true
			}
		case "region": // region <start> <end>: hand-written code, one function enterable anywhere
			if len(fs) < 3 {
				return fmt.Errorf("%s: %q", path, line)
			}
			e, err := strconv.ParseUint(strings.TrimPrefix(fs[2], "0x"), 16, 32)
			if err != nil {
				return fmt.Errorf("%s: %q", path, line)
			}
			p.Regions = append(p.Regions, [2]uint32{uint32(a), uint32(e)})
			p.Extra[uint32(a)] = true
		case "thumb":
			p.Thumb[uint32(a)] = true
			p.hintThumb[uint32(a)] = true
		case "entry":
			p.Extra[uint32(a)] = true
		}
	}
	return sc.Err()
}

// boundsFor returns the body range for an entry: Ghidra's bounds of the
// function containing it (an entry in the middle of a function may use the
// rest of that function).
func (p *Program) boundsFor(a uint32) (uint32, uint32) {
	i := sort.Search(len(p.starts), func(i int) bool { return p.starts[i] > a }) - 1
	if i >= 0 {
		b := p.Bounds[p.starts[i]]
		if a >= b[0] && a < b[1] {
			return b[0], b[1]
		}
	}
	end := p.Im.TextEnd
	if i+1 < len(p.starts) {
		end = p.starts[i+1]
	}
	return a, end
}

func writesReg(in arm.Inst, r int) bool {
	switch in.Kind {
	case arm.DataProc:
		return in.Rd == r && (in.Op < arm.TST || in.Op > arm.CMN)
	case arm.Mem:
		return in.Load && in.Rd == r || in.W && in.Rn == r
	case arm.Mul, arm.Clz, arm.Extend:
		return in.Rd == r
	}
	return false
}

// jumpTable recognises ARM switches:
//
//	cmp rX, #n ; [bhi/bcs default] ; ldr{ls,cc} pc, [pc, rX, lsl #2] ; .word ...
//	cmp rX, #n ; addls pc, pc, rX, lsl #2 ; b default ; b case0 ...
func (p *Program) jumpTable(in arm.Inst) ([]uint32, bool) {
	if in.Thumb {
		return nil, false
	}
	var idx int
	isLdr := in.Kind == arm.Mem && in.Load && in.Size == 4 && in.Rn == 15 && in.P && in.U && !in.W &&
		!in.Off.Imm && in.Off.Shift == arm.LSL && in.Off.Amount == 2
	isAdd := in.Kind == arm.DataProc && in.Op == arm.ADD && in.Rn == 15 && !in.Op2.Imm && !in.Op2.RegShift &&
		in.Op2.Shift == arm.LSL && in.Op2.Amount == 2
	switch {
	case isLdr:
		idx = in.Off.Rm
	case isAdd:
		idx = in.Op2.Rm
	default:
		return nil, false
	}
	guard := in.Cond
	var n uint32
	found := false
	for k := uint32(1); k <= 8; k++ {
		q := p.Im.Inst(in.Addr-4*k, false)
		if q.Kind == arm.DataProc && q.Op == arm.CMP && q.Rn == idx && q.Op2.Imm {
			n = q.Op2.Value
			found = true
			break
		}
		if q.Kind == arm.Branch && !q.Link && q.Cond != 14 && guard == 14 {
			guard = q.Cond ^ 1 // the path that reaches the jump
			continue
		}
		if writesReg(q, idx) || q.WritesPC() {
			return nil, false
		}
	}
	if !found {
		return nil, false
	}
	switch guard {
	case 9: // LS
		n++
	case 3: // CC
	default:
		return nil, false
	}
	if n == 0 || n > 4096 {
		return nil, false
	}
	var t []uint32
	for k := uint32(0); k < n; k++ {
		if isAdd {
			t = append(t, in.Addr+8+k*4)
			continue
		}
		w, ok := p.Im.Word(in.Addr + 8 + k*4)
		if !ok || !p.Im.InText(w) {
			return nil, false
		}
		t = append(t, w)
	}
	return t, true
}

// Walk decodes a function's body from its entry.
func (p *Program) regionOf(a uint32) ([2]uint32, bool) {
	for _, r := range p.Regions {
		if a >= r[0] && a < r[1] {
			return r, true
		}
	}
	return [2]uint32{}, false
}

func (p *Program) Walk(entry uint32) *Func {
	if r, ok := p.regionOf(entry); ok {
		if entry != r[0] {
			return &Func{Entry: entry, Start: r[0], End: r[1], AliasOf: r[0], Name: p.Names[entry],
				Insts: map[uint32]arm.Inst{}, Swept: map[uint32]bool{}, FallInto: map[uint32]bool{}, Labels: map[uint32]bool{},
				Tables: map[uint32][]uint32{}, Calls: map[uint32]bool{r[0]: false}, Returns: true}
		}
		return p.walkRegion(r)
	}
	thumb := p.Thumb[entry]
	start, end := p.boundsFor(entry)
	f := &Func{Entry: entry, Thumb: thumb, Start: start, End: end, Name: p.Names[entry], Insts: map[uint32]arm.Inst{},
		Swept: map[uint32]bool{}, FallInto: map[uint32]bool{}, Labels: map[uint32]bool{entry: true}, Tables: map[uint32][]uint32{}, Calls: map[uint32]bool{}}
	inBody := func(a uint32) bool { return a >= start && a < end }
	todo := []uint32{entry}
	for len(todo) > 0 {
		pc := todo[len(todo)-1]
		todo = todo[:len(todo)-1]
		for {
			if _, done := f.Insts[pc]; done {
				break
			}
			if !inBody(pc) && pc != entry {
				// Ghidra ended the function early (a call it took for
				// no-return, or shared tail code): carry on into the next
				// function as a tail call.
				f.FallInto[pc] = true
				f.Calls[pc] = p.Thumb[pc] || thumb
				if p.Thumb[pc] != thumb {
					f.Problems = append(f.Problems, fmt.Sprintf("falls into a function of the other mode at %08X", pc))
				}
				if !p.NoReturn[pc] {
					f.Returns = true
				}
				break
			}
			in := p.Im.Inst(pc, thumb)
			f.Insts[pc] = in
			if in.Kind == arm.Unknown {
				f.Problems = append(f.Problems, fmt.Sprintf("unknown instruction %08X at %08X", in.Raw, pc))
				break
			}
			p.notePointers(in)
			cont := true
			terminal := in.Cond == 14 // a conditional branch or return falls through
			switch {
			case in.Kind == arm.Branch && in.Link:
				// Decoding goes on after every call: a callee wrongly taken
				// for one that never returns must not cut its caller short.
				f.Calls[in.Target] = thumb != in.Exchange
			case in.Kind == arm.Branch:
				if inBody(in.Target) {
					f.Labels[in.Target] = true
					todo = append(todo, in.Target)
				} else {
					f.Calls[in.Target] = thumb // tail call
					if !p.NoReturn[in.Target] {
						f.Returns = true
					}
				}
				cont = !terminal
			case in.IsReturn():
				f.Returns = true
				cont = !terminal
			case in.WritesPC():
				if t, ok := p.jumpTable(in); ok {
					f.Tables[pc] = t
					for _, a := range t {
						if inBody(a) {
							f.Labels[a] = true
							todo = append(todo, a)
						} else {
							f.Calls[a] = false
							f.Returns = true
						}
					}
					// PC reads 8 ahead: the table starts two words on, and the
					// word between is the default path.
					cont = !terminal
				} else if t, ok := p.constJump(in); ok {
					f.Calls[t&^1] = t&1 != 0 // ldr pc, =target: a long tail call
					if t&1 != 0 {
						p.markThumb(t &^ 1)
					}
					f.Returns = true
					cont = !terminal
				} else if in.Kind == arm.BranchReg && in.Link {
					// indirect call
				} else if in.Kind == arm.BranchReg {
					f.Returns = true // indirect tail call
					cont = !terminal
				} else {
					f.Dynamic = true // computed jump: anywhere in the body, or a tail call
					f.Returns = true
					cont = !terminal
				}
			case in.Kind == arm.Svc && (in.Imm == 0x03 || in.Imm == 0x09):
				cont = !terminal // ExitProcess / ExitThread
			}
			if !cont {
				break
			}
			pc += in.Len
		}
	}
	if f.Dynamic {
		// Every instruction of the body may be a target: decode all of it.
		step := uint32(4)
		if thumb {
			step = 2
		}
		for a := start; a < end; a += step {
			if _, ok := f.Insts[a]; !ok {
				f.Insts[a] = p.Im.Inst(a, thumb)
				f.Swept[a] = true
			}
			f.Labels[a] = true
		}
	}
	return f
}

// constJump: ldr pc, [pc, #imm] from read-only memory.
func (p *Program) constJump(in arm.Inst) (uint32, bool) {
	if in.Kind != arm.Mem || !in.Load || in.Rd != 15 || in.Rn != 15 || !in.Off.Imm || !in.P || in.W || in.Size != 4 {
		return 0, false
	}
	la := in.PC() - in.Off.Value
	if in.U {
		la = in.PC() + in.Off.Value
	}
	if !p.Im.ReadOnly(la) {
		return 0, false
	}
	w, ok := p.Im.Word(la)
	return w, ok && p.Im.InText(w&^1)
}

// walkRegion decodes a whole hand-written block as one function: every word
// is an instruction that may be a target (data words in it decode as junk
// that never runs).
func (p *Program) walkRegion(r [2]uint32) *Func {
	f := &Func{Entry: r[0], Start: r[0], End: r[1], Name: fmt.Sprintf("hand-written block %08X-%08X", r[0], r[1]),
		Insts: map[uint32]arm.Inst{}, Swept: map[uint32]bool{}, FallInto: map[uint32]bool{}, Labels: map[uint32]bool{},
		Tables: map[uint32][]uint32{}, Calls: map[uint32]bool{}, Dynamic: true, Region: true, Returns: true}
	for a := r[0]; a < r[1]; a += 4 {
		in := p.Im.Inst(a, false)
		f.Insts[a] = in
		f.Labels[a] = true
		p.notePointers(in)
		if in.Kind == arm.Branch && (in.Target < r[0] || in.Target >= r[1]) && p.Im.InText(in.Target) {
			f.Calls[in.Target] = in.Exchange
		}
	}
	return f
}

func (p *Program) notePointers(in arm.Inst) {
	if in.Kind == arm.VfpMem && in.Rn == 15 {
		la := in.PC()&^3 - in.Imm
		if in.U {
			la = in.PC()&^3 + in.Imm
		}
		p.literal[la] = true
		if in.Dbl {
			p.literal[la+4] = true
		}
	}
	if in.Kind == arm.Mem && in.Load && in.Rn == 15 && in.Off.Imm {
		la := in.PC() - in.Off.Value
		if in.U {
			la = in.PC() + in.Off.Value
		}
		p.literal[la&^3] = true
	}
	if in.Kind == arm.Mem && in.Load && in.Rn == 15 && in.Off.Imm && in.Rd != 15 && in.Size == 4 {
		la := in.PC() - in.Off.Value
		if in.U {
			la = in.PC() + in.Off.Value
		}
		if w, ok := p.Im.Word(la); ok && p.Im.InText(w) {
			p.Pointers[w] = true
			p.codePointer[w] = true
		}
	}
	if in.Kind == arm.DataProc && in.Rn == 15 && in.Op2.Imm && (in.Op == arm.ADD || in.Op == arm.SUB) && in.Rd != 15 {
		a := in.PC() + in.Op2.Value
		if in.Op == arm.SUB {
			a = in.PC() - in.Op2.Value
		}
		if p.Im.InText(a) {
			p.Pointers[a] = true
		}
	}
}

// Discover walks every function reachable from the entry point, the function
// table, the hints and pointers, until no new entry turns up; then marks
// functions that never return and walks again so calls to them end paths.
func (p *Program) Discover() {
	seeds := []uint32{p.Im.TextStart}
	for a := range p.Bounds {
		seeds = append(seeds, a)
	}
	for a := range p.Extra {
		seeds = append(seeds, a)
	}
	p.dataPointer = map[uint32]bool{}
	for a := p.Im.RoStart; a+4 <= p.Im.DataEnd; a += 4 {
		if w, ok := p.Im.Word(a); ok && p.Im.InText(w) {
			p.Pointers[w] = true
			p.dataPointer[w] = true
		}
	}
	for round := 0; round < 4; round++ {
		p.Funcs = map[uint32]*Func{}
		queue := append([]uint32{}, seeds...)
		walk := func() {
			for len(queue) > 0 {
				a := queue[len(queue)-1]
				queue = queue[:len(queue)-1]
				if _, ok := p.Funcs[a]; ok || !p.Im.InText(a) {
					continue
				}
				f := p.Walk(a)
				p.Funcs[a] = f
				for c, th := range f.Calls {
					if th {
						p.markThumb(c)
					} else {
						p.armCalled[c] = true
						if !p.hintThumb[c] {
							delete(p.Thumb, c)
						}
					}
					queue = append(queue, c)
				}
			}
		}
		walk()
		// Pointers become entries when they land on a known function start
		// or on an instruction already decoded in the same mode.
		p.decodedSet = nil
		for w := range p.Pointers {
			a, th := w&^1, w&1 != 0
			if _, ok := p.Funcs[a]; ok {
				continue
			}
			// A pointer into Thumb code is odd, into ARM code 4-aligned;
			// anything else is a data address.
			cs, ce := p.boundsFor(a)
			_, inKnown := p.Bounds[cs]
			inKnown = inKnown && a >= cs && a < ce
			if !th && a&3 != 0 {
				continue
			}
			if inKnown && th != p.Thumb[cs] {
				continue
			}
			if th && !inKnown && p.looksLikeStart(a, true) {
				p.Thumb[a] = true // Thumb code Ghidra did not find
				queue = append(queue, a)
				continue
			}
			// Outside every known function (code Ghidra missed): any address
			// that decodes as an instruction.
			// Inside one, only what looks like a function start (Ghidra
			// sometimes merges a function into the one before it).
			plausible := p.looksLikeStart(a, false) ||
				!inKnown && p.codePointer[w] && p.Im.Inst(a, false).Kind != arm.Unknown
			if _, ok := p.Bounds[a]; ok || p.decoded(a) || plausible {
				if th {
					p.Thumb[a] = true
				}
				queue = append(queue, a)
			}
		}
		walk()
		// Gap sweep: code no decoded function covers that starts like a
		// function (push {..., lr}) is a function reached only through a
		// computed address.
		covered := map[uint32]bool{}
		for _, f := range p.Funcs {
			for a, in := range f.Insts {
				for k := uint32(0); k < in.Len; k += 2 {
					covered[a+k] = true
				}
			}
		}
		for a := p.Im.TextStart; a+4 <= p.Im.TextEnd; a += 2 {
			if covered[a] || p.literal[a&^3] {
				continue
			}
			if _, ok := p.Funcs[a]; ok {
				continue
			}
			w, _ := p.Im.Word(a)
			if a&3 == 0 && w&0xFFFFC000 == 0xE92D4000 && !p.hintThumb[a] {
				queue = append(queue, a)
			} else if h := p.Im.Half(a); h&0xFF00 == 0xB500 && !p.armCalled[a] {
				if _, known := p.Bounds[a]; !known || p.hintThumb[a] {
					p.Thumb[a] = true
					queue = append(queue, a)
				}
			}
		}
		walk()
		changed := false
		for a, f := range p.Funcs {
			nr := !f.Returns
			if nr != p.NoReturn[a] {
				p.NoReturn[a] = nr
				changed = true
			}
		}
		if !changed {
			break
		}
	}
}

// looksLikeStart: a function prologue (push, sub sp, mov ip sp, bx lr), or
// code right after a return or an unconditional branch.
func (p *Program) looksLikeStart(a uint32, thumb bool) bool {
	if thumb {
		h := p.Im.Half(a)
		if h&0xFF00 == 0xB500 || h&0xFE00 == 0xB400 || h == 0x4770 || h&0xFF80 == 0xB080 {
			return true
		}
		if p.literal[(a-2)&^3] && a&3 == 0 {
			return true
		}
		prev := p.Im.Half(a - 2)
		return prev == 0x4770 || prev&0xFF00 == 0xBD00 || prev&0xF800 == 0xE000
	}
	w, _ := p.Im.Word(a)
	if w&0xFFFF0000 == 0xE92D0000 || w == 0xE12FFF1E || w&0xFFFFF000 == 0xE24DD000 || w == 0xE1A0C00D || w&0xFFFF0FFF == 0xE52D0004 {
		return true
	}
	if p.literal[a-4] {
		return true // after a literal pool
	}
	prev := p.Im.Inst(a-4, false)
	if prev.Kind == arm.Unknown || prev.Cond != 14 {
		return false
	}
	return prev.WritesPC() && !(prev.Kind == arm.Branch && prev.Link) && !(prev.Kind == arm.BranchReg && prev.Link)
}

// markThumb records a Thumb entry unless the address is known ARM: a
// function in Ghidra's table that is not a Thumb one, or a target of ARM
// calls (code mis-decoded as Thumb would otherwise turn ARM functions into
// Thumb ones).
func (p *Program) markThumb(a uint32) {
	if p.hintThumb[a] {
		p.Thumb[a] = true
		return
	}
	if _, known := p.Bounds[a]; known || p.armCalled[a] {
		return
	}
	p.Thumb[a] = true
}

func (p *Program) funcContaining(a uint32) uint32 {
	s, _ := p.boundsFor(a)
	return s
}

func (p *Program) decoded(a uint32) bool {
	if p.decodedSet == nil {
		p.decodedSet = map[uint32]bool{}
		for _, f := range p.Funcs {
			for x := range f.Insts {
				if !f.Swept[x] {
					p.decodedSet[x] = true
				}
			}
		}
	}
	return p.decodedSet[a]
}
