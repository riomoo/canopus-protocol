// ds2recomp: static recompiler for the Devil Survivor 2 Record Breaker
// code.bin (ARMv6K + VFPv2) to C. It runs on the player's own dump; its
// output is never distributed.
package main

import (
	"flag"
	"fmt"
	"os"
	"sort"

	"ds2rb/recomp/arm"
)

func main() {
	code := flag.String("code", "", "code.bin")
	exh := flag.String("exheader", "", "exheader.bin")
	funcs := flag.String("funcs", "", "function table (CSV addr,name,size)")
	hints := flag.String("hints", "", "hints file (thumb / entry lines)")
	out := flag.String("out", "", "output folder for the C files")
	stats := flag.Bool("stats", false, "print decoding statistics only")
	insnTests := flag.String("insntests", "", "write the instruction-level test functions to this C file")
	flag.Parse()
	im, err := LoadImage(*code, *exh)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	p := NewProgram(im)
	if *funcs != "" {
		if err := LoadFuncTable(p, *funcs); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
	if *hints != "" {
		if err := LoadHints(p, *hints); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
	p.Discover()
	if *stats {
		printStats(p)
		return
	}
	if *insnTests != "" {
		if err := GenerateInsnTests(p, *insnTests); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	if err := Generate(p, *out); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func printStats(p *Program) {
	n, probs := 0, 0
	kinds := map[arm.Kind]int{}
	pcw := map[string]int{}
	extra := 0
	var keys []uint32
	for a, f := range p.Funcs {
		keys = append(keys, a)
		n += len(f.Insts)
		if _, ok := p.Bounds[a]; !ok {
			extra++
		}
		for _, in := range f.Insts {
			kinds[in.Kind]++
			if in.WritesPC() && !in.IsReturn() && in.Kind != arm.Branch {
				k := fmt.Sprintf("kind %d", in.Kind)
				if _, ok := f.Tables[in.Addr]; ok {
					k += " table"
				} else if in.Kind == arm.BranchReg && in.Link {
					k += " blx"
				}
				pcw[k]++
				if pcw[k] <= 4 && os.Getenv("SAMPLES") != "" {
					fmt.Printf("sample %s: %08X %08X in %08X %s\n", k, in.Addr, in.Raw, f.Entry, f.Name)
				}
			}
		}
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })
	byProblem := map[string]int{}
	shown := 0
	for _, a := range keys {
		f := p.Funcs[a]
		if len(f.Problems) > 0 {
			probs++
			for _, pr := range f.Problems {
				byProblem[pr[:min(len(pr), 20)]]++
				if shown < 40 && os.Getenv("PROBS") != "" {
					fmt.Printf("%08X %s: %s\n", a, f.Name, pr)
					shown++
				}
			}
		}
	}
	nr := 0
	for _, v := range p.NoReturn {
		if v {
			nr++
		}
	}
	if os.Getenv("NORETURN") != "" {
		for _, a := range keys {
			if p.NoReturn[a] {
				if _, ok := p.Bounds[a]; ok {
					f := p.Funcs[a]
					fmt.Printf("noreturn %08X %s problems=%d\n", a, f.Name, len(f.Problems))
				}
			}
		}
	}
	fmt.Printf("functions %d (%d not in the table), instructions %d, with problems %d, no-return %d\n", len(p.Funcs), extra, n, probs, nr)
	fmt.Println("problems:", byProblem)
	fmt.Println("kinds:", kinds)
	fmt.Println("pc writes:", pcw)
}
