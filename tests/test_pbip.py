#!/usr/bin/env python3
"""Consistency checks for the Power BI project, standard library only.

These catch what schemas can't: a visual naming a field the model lacks, or a model column the
driver doesn't have. They don't prove Power BI Desktop opens the project. That is Gate A.
Run: python3 -B -m unittest tests/test_pbip.py        Print the project's SQL: python3 -B tests/test_pbip.py --print-sql
"""
import json
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJECT = ROOT / "samples" / "powerbi" / "project"
TMDL = PROJECT / "StrataHistorian.SemanticModel" / "definition"


def read_table(path):
    """(columns, measures, m_source) from one .tmdl table file."""
    text = path.read_text(encoding="utf-8")
    columns = re.findall(r"^\tcolumn (.+)$", text, re.M)
    measures = [m.strip("'") for m in re.findall(r"^\tmeasure ('[^']+'|\S+) =", text, re.M)]
    source = text.split("source =\n", 1)[1]
    return columns, measures, source


def tables():
    return {p.stem: read_table(p) for p in (TMDL / "tables").glob("*.tmdl")}


def catalog_columns():
    """Column names per driver table, read from the driver's own catalog source."""
    text = (ROOT / "src" / "core" / "catalog.cpp").read_text(encoding="utf-8")
    result = {}
    for name in ("Points", "RecordedValues"):
        body = text.split(f'TableDef t{{"{name}"', 1)[1].split("}};", 1)[0]
        result[name] = re.findall(r'\{"(\w+)", ColType::', body)
    return result


def parameter_defaults():
    text = (TMDL / "expressions.tmdl").read_text(encoding="utf-8")
    return dict(re.findall(r'^expression (\w+) = ("[^"]*"|#datetime\([^)]*\)) meta', text, re.M))


def resolved_sql(source):
    """Evaluates the project's M string building for its default parameter values."""
    defaults = parameter_defaults()
    def literal(match):
        y, mo, d, h, mi, s = (int(n) for n in re.findall(r"\d+", defaults[match.group(1)]))
        return f"{y:04d}-{mo:02d}-{d:02d} {h:02d}:{mi:02d}:{s:02d}"
    if 'Sql =' not in source:
        return re.search(r'"dsn=" & Dsn, "([^"]+)"', source).group(1) if "Odbc.Query" in source else None
    body = source.split("Sql =", 1)[1].split("Source =", 1)[0]
    body = re.sub(r'" & Literal\((\w+)\) & "', literal, body)
    body = re.sub(r'''" & Text\.Replace\((\w+), "'", "''"\) & "''', lambda m: defaults[m.group(1)].strip('"'), body)
    return "".join(re.findall(r'"([^"]*)"', body))


def visual_fields():
    """(entity, property, kind) for every field a visual uses."""
    found = set()
    def walk(node):
        if isinstance(node, dict):
            for kind in ("Column", "Measure"):
                inner = node.get(kind)
                if isinstance(inner, dict) and "Property" in inner:
                    found.add((inner["Expression"]["SourceRef"]["Entity"], inner["Property"], kind))
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
    for path in PROJECT.rglob("visual.json"):
        walk(json.loads(path.read_text(encoding="utf-8")))
    return found


class Project(unittest.TestCase):
    def test_every_field_a_visual_uses_exists_in_the_model(self):
        model = tables()
        fields = visual_fields()
        self.assertGreater(len(fields), 10)
        for entity, prop, kind in fields:
            columns, measures, _ = model[entity]
            self.assertIn(prop, columns if kind == "Column" else measures, f"{entity}.{prop}")

    def test_measures_only_use_fields_that_exist(self):
        model = tables()
        text = (TMDL / "tables" / "PressureEvents.tmdl").read_text(encoding="utf-8")
        for entity, column in re.findall(r"(PressureEvents|RefreshStamp|RunEvents|Points)\[(\w+)\]", text):
            self.assertIn(column, model[entity][0], f"{entity}[{column}]")

    def test_sql_selects_exactly_the_model_columns_and_they_exist_in_the_driver(self):
        catalog = catalog_columns()
        for name, (columns, _, source) in tables().items():
            sql = resolved_sql(source)
            if name == "RefreshStamp":
                self.assertIsNone(sql)
                continue
            selected, table = re.match(r"SELECT (.+?) FROM (\w+)", sql).groups()
            selected = [c.strip() for c in selected.split(",")]
            self.assertEqual(selected, columns, name)
            self.assertLessEqual(set(selected), set(catalog[table]), name)

    def test_event_reads_are_bounded_and_exclusive_at_the_end(self):
        for name in ("PressureEvents", "RunEvents"):
            sql = resolved_sql(tables()[name][2])
            self.assertRegex(sql, r"WHERE TagName = 'SYN\.[\w.]+' AND EventUtc >= \{ts '[\d: -]+'\} AND EventUtc < \{ts '[\d: -]+'\}$")

    def test_pressure_chart_averages_rather_than_sums(self):
        path = next(PROJECT.rglob("pressure_trend/visual.json"))
        projection = json.loads(path.read_text(encoding="utf-8"))["visual"]["query"]["queryState"]["Y"]["projections"][0]
        self.assertEqual(projection["field"]["Aggregation"]["Function"], 1)  # 0 is Sum, and repeated timestamps would spike

    def test_relationships_use_real_columns(self):
        model = tables()
        text = (TMDL / "relationships.tmdl").read_text(encoding="utf-8")
        pairs = re.findall(r"(?:from|to)Column: (\w+)\.(\w+)", text)
        self.assertEqual(len(pairs), 4)
        for table, column in pairs:
            self.assertIn(column, model[table][0])

    def test_files_are_utf8_without_bom_and_hold_no_credentials(self):
        for path in PROJECT.rglob("*"):
            if not path.is_file():
                continue
            raw = path.read_bytes()
            self.assertFalse(raw.startswith(b"\xef\xbb\xbf"), path.name)
            text = raw.decode("utf-8")
            self.assertNotRegex(text, r"(?i)\b(pwd|password|uid)\s*=", path.name)

    def test_project_paths_fit_the_windows_limit(self):
        for path in PROJECT.rglob("*"):
            relative = str(path.relative_to(ROOT))
            self.assertLess(len(r"C:\strata-odbc-0.2.0-complete-UNSIGNED" + "\\" + relative), 260, relative)


if __name__ == "__main__":
    if "--print-sql" in sys.argv:
        for name, (_, _, source) in sorted(tables().items()):
            sql = resolved_sql(source)
            if sql:
                print(sql)
        print("quit")
    else:
        unittest.main()
