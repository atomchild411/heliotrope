#!/usr/bin/env python3
"""ssprof.py DUMP.txt EXPFILE [LIB=UNSTRIPPED ...]: where a SpeedShop pcsamp
profile of the Web process spends its time.

DUMP.txt is `ssdump EXPFILE` (made on IRIX); EXPFILE the raw experiment.
The PC histograms are compressed 16-bit arrays, one per DSO: a 0 followed by
N skips N buckets, anything else is a count for the next bucket; a bucket
is one 4-byte instruction from the DSO's link-time address. Each LIB given
(installed name = unstripped copy) is symbolized with llvm-nm.

Prints samples per DSO, the top functions, and for libWPEWebKit a split
into the JavaScript interpreter (CLoop::execute), its slow paths, the rest
of JavaScriptCore by area, and WebCore / WTF / other.
"""
import bisect
import collections
import re
import struct
import subprocess
import sys

NM = "/Volumes/portable/workspace-claude/IRIX/scratch/llvm-irix/cross21/bin/llvm-nm"


def parse_dump(path):
    text = open(path).read()
    objs = {}
    # the last program object list is the complete one
    lists = text.split("Program Object List")
    for block in lists[1:]:
        for m in re.finditer(r"Program Object (\d+), Named `([^']+)'\s+Link Time Address: 0x([0-9a-f]+)\s+"
                             r"Run Time Address: 0x([0-9a-f]+)\s+Size: 0x([0-9a-f]+)", block):
            objs[int(m.group(1))] = (m.group(2), int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16))
    recs = []
    for m in re.finditer(r"Compressed PC sampling array \(16-bit\)\s+\d+, offset (\d+) = 0x[0-9a-f]+ \(size\s+(\d+)\)\s+"
                         r"compressed short array, dso index = (-?\d+), array size = (\d+)", text):
        recs.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    return objs, recs


def decode(raw, recs):
    hist = {}
    for off, size, dso in recs:
        r = raw[off:off + size]
        n = (size - 16) // 2
        sh = struct.unpack(">%dH" % n, r[16:16 + n * 2])
        i = b = 0
        counts = collections.Counter()
        while i < len(sh):
            v = sh[i]
            if v == 0:
                if i + 1 < len(sh):
                    b += sh[i + 1]
                i += 2
            else:
                counts[b] += v
                b += 1
                i += 1
        hist[dso] = counts
    return hist


def symbols(lib):
    out = subprocess.run([NM, "-C", "-n", "--defined-only", lib], capture_output=True, text=True).stdout
    if not re.search(r" [tT] ", out):  # stripped: the dynamic symbols (IRIX's libc keeps them)
        out = subprocess.run([NM, "-C", "-n", "-D", "--defined-only", lib], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        # "?": IRIX's sections that llvm-nm does not classify
        if len(parts) == 3 and parts[1] in "tTwW?":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


TEXT = re.compile(r"WebCore::(Font|FontCascade|FontCache|FontPlatformData|FontRanges|FontSelector|CSSFontSelector|"
                  r"Glyph|WidthIterator|ComplexText|TextRun|TextBreakIterator|CachedLineBreakIterator|"
                  r"LazyLineBreakIterator|TextPainter|SimpleFontData|FontFamily|FontDescription|"
                  r"Layout::TextUtil|Layout::InlineTextItem|TextBoxPainter|SurrogatePair|FontVariant|"
                  r"HarfBuzz|Cairo.*Font|FontCustomPlatformData|SkiaHarfBuzz|OpenType)|hb_|FT_|Fc[A-Z]")


def area(name):
    if TEXT.search(name):
        return "text: fonts, shaping, measuring, drawing glyphs"
    if "CLoop::execute" in name:
        return "JS interpreter (CLoop::execute)"
    if re.search(r"JSC::LLInt::|llint_|slow_path|JSC::CommonSlowPaths", name):
        return "JS interpreter slow paths"
    if re.search(r"JSC::(Parser|Lexer|BytecodeGenerator|.*Node::emit|ASTBuilder|UnlinkedCodeBlock|CodeBlock|"
                 r"Scope|SourceProvider|BytecodeRewriter|.*Generator)", name):
        return "JS parse + bytecode"
    if re.search(r"JSC::(Heap|MarkedBlock|MarkedSpace|SlotVisitor|AbstractSlotVisitor|Subspace|BlockDirectory|"
                 r"LocalAllocator|GCClient|IsoCellSet|PreciseAllocation|WeakSet|HandleSet|.*Marking)", name):
        return "JS garbage collection"
    if re.search(r"^(JSC::|operation|Yarr|JSC)", name) or "Yarr" in name:
        return "JS runtime (objects, strings, regexps, builtins)"
    if name.startswith("WebCore::") or "WebCore" in name:
        return "WebCore (DOM, style, layout, painting)"
    if name.startswith("WTF::") or name.startswith("bmalloc") or name.startswith("pas_"):
        return "WTF / allocator"
    if name.startswith("WebKit::") or name.startswith("IPC::"):
        return "WebKit (IPC, process glue)"
    return "other"


def main():
    dump, exp = sys.argv[1], sys.argv[2]
    libs = dict(a.split("=", 1) for a in sys.argv[3:])
    objs, recs = parse_dump(dump)
    hist = decode(open(exp, "rb").read(), recs)
    total = sum(sum(c.values()) for c in hist.values())
    print("total %d samples (%.1f s at 10 ms)" % (total, total / 100))
    per = sorted(((sum(c.values()), d) for d, c in hist.items()), reverse=True)
    for s, d in per[:12]:
        print("  %6d %5.1f%%  %s" % (s, 100.0 * s / total, objs.get(d, ("?",))[0]))
    for d, (name, link, run, size) in sorted(objs.items()):
        if name not in libs or d not in hist:
            continue
        addrs, names = symbols(libs[name])
        funcs = collections.Counter()
        for b, c in hist[d].items():
            a = link + 4 * b
            i = bisect.bisect_right(addrs, a) - 1
            funcs[names[i] if i >= 0 else "?"] += c
        lib_total = sum(funcs.values())
        print("\n%s: %d samples (%.1f%% of all)" % (name, lib_total, 100.0 * lib_total / total))
        areas = collections.Counter()
        for f, c in funcs.items():
            areas[area(f)] += c
        for a, c in areas.most_common():
            print("  %6d %5.1f%%  %s" % (c, 100.0 * c / total, a))
        print("  top functions:")
        for f, c in funcs.most_common(25):
            print("  %6d %5.1f%%  %s" % (c, 100.0 * c / total, f[:110]))


main()
