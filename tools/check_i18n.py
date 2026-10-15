#!/usr/bin/env python3
"""Every T("…"), N_("…") and L("…") text in the sources must have an Azerbaijani translation in
core/I18n_az.cpp, and the %1, %2 … placeholders must match. Exit code 1 lists what is missing.
   tools/check_i18n.py [--list]   (--list prints the missing source texts as C++ lines)"""
import os, re, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
lit = r'"((?:[^"\\]|\\.)*)"'
use = re.compile(r'\b(?:T|N_|L)\(\s*' + lit)
pair = re.compile(r'\{\s*' + lit + r'\s*,\s*' + lit + r'\s*\}')

def unescape(s):
    return bytes(s, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')

used = {}
for d in ('core', 'cli', 'gui'):
    for dirpath, _, files in os.walk(os.path.join(root, d)):
        for f in files:
            if not f.endswith(('.cpp', '.h')) or f == 'I18n_az.cpp':
                continue
            text = open(os.path.join(dirpath, f), encoding='utf-8').read()
            for m in use.finditer(text):
                used.setdefault(m.group(1), f)

dic = {}
for m in pair.finditer(open(os.path.join(root, 'core', 'I18n_az.cpp'), encoding='utf-8').read()):
    dic[m.group(1)] = m.group(2)

missing = [s for s in used if s not in dic]
bad = [s for s in used if s in dic and sorted(re.findall(r'%\d', s)) != sorted(re.findall(r'%\d', dic[s]))]
if '--list' in sys.argv:
    for s in missing:
        print('    {"%s", ""},' % s)
    sys.exit(0)
for s in missing:
    print('missing translation (%s): %s' % (used[s], s))
for s in bad:
    print('placeholder mismatch: %s  ->  %s' % (s, dic[s]))
print('%d texts, %d missing, %d mismatched' % (len(used), len(missing), len(bad)))
sys.exit(1 if missing or bad else 0)
