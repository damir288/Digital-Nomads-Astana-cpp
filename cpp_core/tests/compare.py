"""Сравнение результатов C++-ядра с эталоном Python.

    python3 tests/compare.py build/station_core tools/data [--cpsat[=секунды]]

Для каждого снимка tools/data/<case>.snapshot.json запускает `station_core golden` и сравнивает
вывод с <case>.golden.json: каждое поле, каждую строку объяснения, каждый список. Числа сравниваются
с допуском 1e-9 (целое 5 и дробное 5.0 считаются равными). Печатает первые расхождения.
"""
import json
import subprocess
import sys
from pathlib import Path


def diff(a, b, path, out, limit=15):
    if len(out) >= limit:
        return
    if isinstance(a, bool) or isinstance(b, bool):
        if a is not b:
            out.append(f"{path}: python={a!r} c++={b!r}")
        return
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if abs(a - b) > 1e-9 * max(1.0, abs(a)):
            out.append(f"{path}: python={a!r} c++={b!r}")
        return
    if isinstance(a, dict) and isinstance(b, dict):
        for k in a.keys() | b.keys():
            if k not in a or k not in b:
                out.append(f"{path}.{k}: есть только в {'python' if k in a else 'c++'}")
            else:
                diff(a[k], b[k], f"{path}.{k}", out, limit)
        return
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append(f"{path}: длина python={len(a)} c++={len(b)}")
        for i, (x, y) in enumerate(zip(a, b)):
            diff(x, y, f"{path}[{i}]", out, limit)
        return
    if a != b:
        out.append(f"{path}: python={a!r}\n{' ' * (len(path) + 2)}c++   ={b!r}")


def count_leaves(x):
    if isinstance(x, dict):
        return sum(count_leaves(v) for v in x.values())
    if isinstance(x, list):
        return sum(count_leaves(v) for v in x)
    return 1


def main(binary, data_dir):
    cases = sorted(Path(data_dir).glob("*.snapshot.json"))
    if not cases:
        print("Нет снимков в", data_dir)
        return 1
    failed = 0
    total_leaves = 0
    for snap in cases:
        name = snap.name.replace(".snapshot.json", "")
        gold = json.loads((snap.parent / f"{name}.golden.json").read_text(encoding="utf-8"))
        run = subprocess.run([binary, "golden", str(snap)], capture_output=True, text=True)
        if run.returncode != 0:
            print(f"✗ {name}: программа завершилась с ошибкой\n{run.stderr}")
            failed += 1
            continue
        cpp = json.loads(run.stdout)
        gold.pop("cpsat", None)  # CP-SAT сравнивается отдельно (--cpsat)
        out = []
        diff(gold, cpp, name, out)
        leaves = count_leaves(gold)
        total_leaves += leaves
        if out:
            failed += 1
            print(f"✗ {name}: расхождения ({len(out)}{'+' if len(out) >= 15 else ''}):")
            for line in out:
                print("   ", line)
        else:
            stats = (f"конфликтов {len(gold['conflicts'])}, заявок {len(gold['checks'])}, "
                     f"операций в плане {len(gold['greedy']['schedule'])}")
            print(f"✓ {name}: совпадает полностью ({leaves} значений; {stats})")
    print()
    print(f"Итог: {len(cases) - failed} из {len(cases)} случаев совпадают с Python, сравнено {total_leaves} значений.")
    if cpsat_limit:
        failed += compare_cpsat(binary, cases, cpsat_limit)
    return 1 if failed else 0


def compare_cpsat(binary, cases, limit):
    """CP-SAT: при доказанной оптимальности значения цели должны совпасть; решение должно так же,
    как в Python, проходить (или не проходить) независимую проверку плана."""
    print()
    print(f"CP-SAT (лимит {limit} с, 8 потоков):")
    bad = 0
    for snap in cases:
        name = snap.name.replace(".snapshot.json", "")
        gold = json.loads((snap.parent / f"{name}.golden.json").read_text(encoding="utf-8")).get("cpsat")
        if not gold:
            continue
        run = subprocess.run([binary, "cpsat", str(snap), str(limit), "8"], capture_output=True, text=True)
        if run.returncode != 0:
            print(f"✗ {name}: {run.stderr.strip()}")
            bad += 1
            continue
        cpp = json.loads(run.stdout)
        py_ok, cpp_ok = not gold["verify"], not cpp["verify"]
        line = (f"{name:14s} python: {gold['status']:8s} цель {gold['objective']!s:8s} проверка {'да' if py_ok else 'нет'} | "
                f"c++: {cpp['status']:8s} цель {cpp['objective']!s:8s} проверка {'да' if cpp_ok else 'нет'}")
        problem = None
        if gold["status"] == "optimal" and cpp["status"] == "optimal" and abs(gold["objective"] - cpp["objective"]) > 1e-6:
            problem = "оптимумы различаются"
        elif gold["status"] == "optimal" and cpp["status"] == "feasible" and cpp["objective"] < gold["objective"] - 1e-6:
            problem = "c++ нашёл значение лучше доказанного оптимума Python"
        elif cpp["status"] == "optimal" and gold["status"] == "feasible" and gold["objective"] < cpp["objective"] - 1e-6:
            problem = "python нашёл значение лучше доказанного оптимума C++"
        elif gold["status"] == "optimal" and cpp["status"] == "optimal" and py_ok != cpp_ok:
            problem = "результат проверки плана различается"
        print(("✗ " if problem else "✓ ") + line + (f"  <- {problem}" if problem else ""))
        bad += 1 if problem else 0
    return bad


cpsat_limit = 0.0

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--cpsat")]
    for a in sys.argv[1:]:
        if a.startswith("--cpsat"):
            cpsat_limit = float(a.split("=", 1)[1]) if "=" in a else 20.0
    sys.exit(main(args[0] if args else "build/station_core", args[1] if len(args) > 1 else "tools/data"))
