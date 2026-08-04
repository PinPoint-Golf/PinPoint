#!/usr/bin/env python3
"""Regenerate the per-metric sign table in the metric catalogue developer guide.

The table is GENERATED, never hand-edited: it restates signPositive / signNegative from
metric_catalogue_manifest.cpp, and a hand-maintained second copy of 86 facts is exactly the
drift the manifest header warns about. Change the descriptor, then run this.

    python3 tools/metrics/gen_sign_table.py

Rewrites the block between the two marker comments in
docs/developer/metric_catalogue_developer_guide.md and leaves the rest of the file alone.
"""
import re, collections, pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC  = ROOT / "src/Metrics/metric_catalogue_manifest.cpp"
DOC  = ROOT / "docs/developer/metric_catalogue_developer_guide.md"
BEGIN = "<!-- BEGIN GENERATED SIGN TABLE -->"
END   = "<!-- END GENERATED SIGN TABLE -->"


def descriptors(text):
    starts = [m.start() for m in re.finditer(r"    cat\.addDescriptor\(\{", text)]
    for i, st in enumerate(starts):
        en = starts[i + 1] if i + 1 < len(starts) else len(text)
        yield text[st:en]


def literal(chunk, name):
    m = re.search(r"\.%s = QStringLiteral\((.*?)\),\n        \." % name, chunk, re.S)
    return " ".join(re.findall(r'"([^"]*)"', m.group(1))) if m else ""


def main():
    text = SRC.read_text()
    groups = collections.OrderedDict()
    for c in descriptors(text):
        key = re.search(r'\.key = QStringLiteral\("([^"]+)"\)', c).group(1)
        grp = re.search(r'\.group = QStringLiteral\("([^"]*)"\)', c)
        unit = re.search(r'\.unit = QStringLiteral\("([^"]*)"\)', c)
        planned = "PLANNED)" in c and c.count("via(") == c.count("PLANNED)")
        device = ".launchMonitor = true" in c
        status = "device" if (device and not planned) else ("planned" if planned else "live")
        groups.setdefault(grp.group(1) if grp else "", []).append(
            dict(key=key, unit=(unit.group(1) if unit else ""), status=status,
                 pos=literal(c, "signPositive"), neg=literal(c, "signNegative")))

    out = [BEGIN, ""]
    for grp, rows in groups.items():
        out += ["#### %s" % grp, "",
                "| Metric | Unit | Status | Positive means | Negative means |",
                "|---|---|---|---|---|"]
        for r in rows:
            out.append("| `%s` | %s | %s | %s | %s |" % (
                r["key"], r["unit"] or "—", r["status"],
                r["pos"] or "*no direction*",
                r["neg"] or "*cannot go negative*"))
        out.append("")
    out.append(END)

    doc = DOC.read_text()
    if BEGIN not in doc or END not in doc:
        sys.exit("markers not found in %s" % DOC)
    head, rest = doc.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    DOC.write_text(head + "\n".join(out) + tail)
    print("wrote %d metrics across %d groups" % (sum(len(v) for v in groups.values()), len(groups)))

    # Appendix A is hand-maintained and claims to be exhaustive by construction. It went stale once
    # already — nine rows kept their pre-rename keys and twenty-five descriptors had no row at all —
    # so the claim is checked here rather than trusted. Not a unit test because no test in this repo
    # reads markdown, and inventing that category for one table is worse than a script that says so.
    every = {r["key"] for rows in groups.values() for r in rows}
    doc = DOC.read_text()
    appendix = doc[doc.index("## Appendix A — per-metric work plan"):]
    listed = set(re.findall(r"^\| `([^`]+)` \|", appendix, re.M))
    stale, missing = sorted(listed - every), sorted(every - listed)
    if stale or missing:
        print("\nAppendix A is out of step with the manifest:")
        for k in stale:   print("  STALE   row for `%s`, which is no longer a descriptor" % k)
        for k in missing: print("  MISSING row for `%s`" % k)
        sys.exit(1)
    print("Appendix A: %d rows, exhaustive" % len(listed))


if __name__ == "__main__":
    main()
