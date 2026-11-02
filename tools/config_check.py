#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""Check a settings file against config/rdof-config.schema.json (version 3).

    tools/config_check.py FILE [FILE ...]

Uses the jsonschema package (MIT) when it's installed, else node with config/schema-check.js. The schema checks
keys, types and ranges; the C parsers also check names, ownership and what fits the platform (docs/spec/fw/
config-v3.md). Comments (// and /* */, as the board takes) are allowed. Exit 0 when every file passes.
"""
import json
import os
import shutil
import subprocess
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA = os.path.join(TOP, "config", "rdof-config.schema.json")


def jsonc(text):
    """The JSON in text, its // and /* */ comments (outside strings) taken out."""
    out, i, n, string = [], 0, len(text), False
    while i < n:
        c = text[i]
        if string:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 1
            elif c == '"':
                string = False
        elif c == '"':
            string = True
            out.append(c)
        elif text.startswith("//", i):
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        elif text.startswith("/*", i):
            i = text.find("*/", i + 2)
            if i < 0:
                break
            i += 1
        else:
            out.append(c)
        i += 1
    return json.loads("".join(out))


def errors_jsonschema(schema, doc):
    import jsonschema

    v = jsonschema.Draft202012Validator(schema)
    out = []
    for e in sorted(v.iter_errors(doc), key=lambda e: list(e.path)):
        path = ".".join(str(p) if isinstance(p, str) else "[%d]" % p for p in e.path).replace(".[", "[")
        out.append((path, e.message))
    return out


def errors_node(doc):
    node = shutil.which("node")
    if not node:
        sys.exit("config_check: needs the jsonschema package (pip install jsonschema) or node")
    js = (
        "const {check}=require(process.argv[1]);const s=require(process.argv[2]);"
        "let d='';process.stdin.on('data',c=>d+=c).on('end',()=>"
        "console.log(JSON.stringify(check(s,JSON.parse(d)))));"
    )
    r = subprocess.run(
        [node, "-e", js, os.path.join(TOP, "config", "schema-check.js"), SCHEMA],
        input=json.dumps(doc), capture_output=True, text=True, check=True,
    )
    return [(e["path"], e["message"]) for e in json.loads(r.stdout)]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    with open(SCHEMA) as f:
        schema = json.load(f)
    bad = 0
    for name in sys.argv[1:]:
        try:
            with open(name) as f:
                doc = jsonc(f.read())
        except (OSError, ValueError) as e:
            print("%s: %s" % (name, e))
            bad += 1
            continue
        if not isinstance(doc, dict) or doc.get("version") != 3:
            print("%s: not version 3 settings" % name)
            bad += 1
            continue
        try:
            errs = errors_jsonschema(schema, doc)
        except ImportError:
            errs = errors_node(doc)
        for path, msg in errs:
            print("%s: %s: %s" % (name, path or "(top)", msg))
        if errs:
            bad += 1
        else:
            print("%s: ok" % name)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
