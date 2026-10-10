#!/usr/bin/env python3
"""Keep framebuffer probe fixtures within the kernel heap's largest class."""
from pathlib import Path
import ast
import operator
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "src/kernel"
MAX_CLASS = 2048


def main():
    ops = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
           ast.FloorDiv: operator.floordiv, ast.Div: operator.floordiv}

    def evaluate(expr, defines, trail=()):
        expr = expr.strip().rstrip("uUlL")
        expr = re.sub(r"\b([A-Za-z_]\w*)\b", lambda m: "(" + str(evaluate(defines[m[1]], defines, trail + (m[1],))) + ")"
                      if m[1] in defines and m[1] not in trail else m[1], expr)
        tree = ast.parse(expr, mode="eval").body

        def visit(node):
            if isinstance(node, ast.Constant) and isinstance(node.value, int):
                return node.value
            if isinstance(node, ast.BinOp) and type(node.op) in ops:
                return ops[type(node.op)](visit(node.left), visit(node.right))
            raise ValueError
        return visit(tree)

    paths = sorted((KERNEL / "probes").glob("*.c")) + sorted((KERNEL / "drivers").glob("*_probe.c"))
    violations = []
    for path in paths:
        source = path.read_text()
        defines = {name: value.strip().rstrip("uUlL")
                   for name, value in re.findall(r"^#define\s+(\w+)\s+(.+)$", source, re.M)}
        for match in re.finditer(r"\bkmalloc\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)", source):
            try:
                size = evaluate(match.group(1), defines)
            except (KeyError, SyntaxError, ValueError, ZeroDivisionError):
                continue
            if size > MAX_CLASS:
                line = source.count("\n", 0, match.start()) + 1
                violations.append((path.relative_to(ROOT).as_posix(), line, size))
    if violations:
        for path, line, size in violations:
            print(f"FAIL probe fixture allocation: {path}:{line} requests {size} bytes; heap maximum is {MAX_CLASS}", file=sys.stderr)
        return 1
    print(f"PASS framebuffer fixture allocation sizes: no constant kmalloc request exceeds {MAX_CLASS} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
