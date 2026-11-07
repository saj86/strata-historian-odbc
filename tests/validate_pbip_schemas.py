#!/usr/bin/env python3
"""Validates the Power BI project's JSON files against Microsoft's published schemas.

Needs jsonschema and network access to raw.githubusercontent.com. Proves the files match the
schemas, not that Power BI Desktop opens them. Usage: validate_pbip_schemas.py <project folder>
"""
import json
import pathlib
import sys
import urllib.request

import jsonschema
from referencing import Registry, Resource
from referencing.jsonschema import DRAFT7

PREFIX = "https://developer.microsoft.com/json-schemas/"
RAW = "https://raw.githubusercontent.com/microsoft/json-schemas/main/"


def retrieve(uri):
    url = uri.split("#")[0]
    if not url.startswith(PREFIX):
        raise ValueError(f"unexpected schema location: {url}")
    with urllib.request.urlopen(RAW + url[len(PREFIX):], timeout=30) as response:
        return Resource.from_contents(json.load(response), default_specification=DRAFT7)


registry = Registry(retrieve=retrieve)
root = pathlib.Path(sys.argv[1])
files = sorted(p for p in root.rglob("*") if p.is_file() and p.suffix in {".json", ".pbir", ".pbism", ".pbip"})
failures = 0
for path in files:
    document = json.loads(path.read_text(encoding="utf-8"))
    schema = registry.get_or_retrieve(document["$schema"])
    registry = schema.registry
    validator = jsonschema.Draft7Validator(schema.value.contents, registry=registry)
    errors = sorted(validator.iter_errors(document), key=lambda e: list(e.path))
    for e in errors[:5]:
        print(f"FAIL {path.relative_to(root)}: {'/'.join(map(str, e.path))}: {e.message[:300]}")
    failures += bool(errors)
print(f"schema validation: {len(files)} files, {failures} with errors")
sys.exit(1 if failures else 0)
