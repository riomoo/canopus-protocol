// recomp: static recompiler for the Devil Survivor 2 Record Breaker
// code.bin (ARMv6K + VFPv2) to C. It runs on the player's own dump; its
// output is never distributed.
package main

import "core:fmt"
import "core:os"
import "core:slice"
import "core:strings"

import "arm"

USAGE :: `usage: recomp -code code.bin -exheader exheader.bin [-funcs table.csv] [-hints hints.txt]
              (-out folder | -stats | -insntests file.c)
  -code       code.bin
  -exheader   exheader.bin
  -funcs      function table (CSV addr,name,size)
  -hints      hints file (thumb / entry / reltable / region lines)
  -out        output folder for the C files
  -stats      print decoding statistics only
  -insntests  write the instruction-level test functions to this C file
`

fail :: proc(msg: string) -> ! {
	fmt.eprintln(msg)
	os.exit(1)
}

main :: proc() {
	code, exh, funcs, hints, out_dir, insn_tests: string
	stats := false
	args := os.args[1:]
	for i := 0; i < len(args); i += 1 {
		a := args[i]
		if !strings.has_prefix(a, "-") {
			fail(fmt.tprintf("unexpected argument %q\n%s", a, USAGE))
		}
		name := strings.trim_left(a, "-")
		value := ""
		has_value := false
		if eq := strings.index_byte(name, '='); eq >= 0 {
			name, value, has_value = name[:eq], name[eq + 1:], true
		}
		take :: proc(args: []string, i: ^int, name: string, value: string, has_value: bool) -> string {
			if has_value {
				return value
			}
			if i^ + 1 >= len(args) {
				fail(fmt.tprintf("flag needs an argument: -%s\n%s", name, USAGE))
			}
			i^ += 1
			return args[i^]
		}
		switch name {
		case "code":
			code = take(args, &i, name, value, has_value)
		case "exheader":
			exh = take(args, &i, name, value, has_value)
		case "funcs":
			funcs = take(args, &i, name, value, has_value)
		case "hints":
			hints = take(args, &i, name, value, has_value)
		case "out":
			out_dir = take(args, &i, name, value, has_value)
		case "insntests":
			insn_tests = take(args, &i, name, value, has_value)
		case "stats":
			stats = !has_value || value == "true" || value == "1"
		case "h", "help":
			fmt.print(USAGE)
			return
		case:
			fail(fmt.tprintf("unknown flag -%s\n%s", name, USAGE))
		}
	}
	im, err := load_image(code, exh)
	if err != "" {
		fail(err)
	}
	p := new_program(im)
	if funcs != "" {
		if e := load_func_table(p, funcs); e != "" {
			fail(e)
		}
	}
	if hints != "" {
		if e := load_hints(p, hints); e != "" {
			fail(e)
		}
	}
	discover(p)
	if stats {
		print_stats(p)
		return
	}
	if insn_tests != "" {
		if e := generate_insn_tests(p, insn_tests); e != "" {
			fail(e)
		}
		return
	}
	if out_dir == "" {
		fail(fmt.tprintf("no -out folder\n%s", USAGE))
	}
	if e := generate(p, out_dir); e != "" {
		fail(e)
	}
}

// print_map prints a count table as "map[k:v k:v]" with the keys in order.
@(private = "file")
print_map :: proc(label: string, m: map[string]int) {
	keys := make([dynamic]string, 0, len(m), context.temp_allocator)
	for k in m {
		append(&keys, k)
	}
	slice.sort(keys[:])
	fmt.print(label, " map[", sep = "")
	for k, i in keys {
		if i > 0 {
			fmt.print(" ")
		}
		fmt.printf("%s:%d", k, m[k])
	}
	fmt.println("]")
}

print_stats :: proc(p: ^Program) {
	n, probs := 0, 0
	kinds: [len(arm.Kind)]int
	pcw: map[string]int
	extra := 0
	samples := os.get_env("SAMPLES", context.temp_allocator) != ""
	keys := sorted_keys(p.funcs)
	for a in keys {
		f := p.funcs[a]
		n += len(f.insts)
		if a not_in p.bounds {
			extra += 1
		}
		for _, in_ in f.insts {
			kinds[in_.kind] += 1
			if arm.writes_pc(in_) && !arm.is_return(in_) && in_.kind != .Branch {
				k := fmt.aprintf("kind %d", int(in_.kind))
				if in_.addr in f.tables {
					k = fmt.aprintf("%s table", k)
				} else if in_.kind == .BranchReg && in_.link {
					k = fmt.aprintf("%s blx", k)
				}
				pcw[k] += 1
				if pcw[k] <= 4 && samples {
					fmt.printf("sample %s: %08X %08X in %08X %s\n", k, in_.addr, in_.raw, f.entry, f.name)
				}
			}
		}
	}
	by_problem: map[string]int
	shown := 0
	show_probs := os.get_env("PROBS", context.temp_allocator) != ""
	for a in keys {
		f := p.funcs[a]
		if len(f.problems) > 0 {
			probs += 1
			for pr in f.problems {
				by_problem[pr[:min(len(pr), 20)]] += 1
				if shown < 40 && show_probs {
					fmt.printf("%08X %s: %s\n", a, f.name, pr)
					shown += 1
				}
			}
		}
	}
	nr := 0
	for _, v in p.no_return {
		if v {
			nr += 1
		}
	}
	if os.get_env("NORETURN", context.temp_allocator) != "" {
		for a in keys {
			if p.no_return[a] && a in p.bounds {
				f := p.funcs[a]
				fmt.printf("noreturn %08X %s problems=%d\n", a, f.name, len(f.problems))
			}
		}
	}
	fmt.printf("functions %d (%d not in the table), instructions %d, with problems %d, no-return %d\n", len(p.funcs), extra, n, probs, nr)
	print_map("problems:", by_problem)
	fmt.print("kinds: map[")
	first := true
	for c, k in kinds {
		if c == 0 {
			continue
		}
		if !first {
			fmt.print(" ")
		}
		first = false
		fmt.printf("%d:%d", int(k), c)
	}
	fmt.println("]")
	print_map("pc writes:", pcw)
}
