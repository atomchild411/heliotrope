#!/usr/bin/env python3
"""make-rules.py DOMAINS.txt OUT.json: a WebKit content-blocker rule list that
blocks third-party loads from each listed domain and its subdomains.

The domain list is Peter Lowe's ad and tracking server list
(https://pgl.yoyo.org/adservers/, plain non-HTML format)."""
import json, re, sys

domains = []
for line in open(sys.argv[1]):
    d = line.strip().lower()
    if not d or d.startswith('#') or not re.fullmatch(r'[a-z0-9.-]+', d):
        continue
    domains.append(d)
domains = sorted(set(domains))
rules = [{
    "trigger": {
        "url-filter": "^[a-z]+://([^/:]+\\.)?" + re.escape(d).replace('\\-', '-') + "[:/]",
        "load-type": ["third-party"],
    },
    "action": {"type": "block"},
} for d in domains]
json.dump(rules, open(sys.argv[2], 'w'), separators=(',', ':'))
print(f"{len(rules)} rules")
