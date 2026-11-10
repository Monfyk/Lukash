#!/usr/bin/env python3
"""Збирає офлайн-версію index.html: шрифти Google Fonts вбудовуються як data:URI,
щоб EXE та APK працювали без інтернету. Результат: packaging/out/www/index.html"""
import base64, os, re, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "packaging", "out", "www")
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36"
KEEP = ("cyrillic", "latin")  # лише потрібні підмножини

def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()

def main():
    html = open(os.path.join(ROOT, "index.html"), encoding="utf-8").read()
    m = re.search(r'<link rel="stylesheet" href="(https://fonts\.googleapis\.com/[^"]+)">', html)
    if not m:
        sys.exit("Не знайшов посилання на Google Fonts в index.html")
    css = get(m.group(1).replace("&amp;", "&")).decode()
    faces = re.findall(r"/\* ([a-z-]+) \*/\s*(@font-face \{.*?\})", css, re.S)
    cache, out = {}, []
    for subset, face in faces:
        if subset not in KEEP:
            continue
        url = re.search(r"url\((https://[^)]+)\)", face).group(1)
        if url not in cache:
            cache[url] = "data:font/woff2;base64," + base64.b64encode(get(url)).decode()
        out.append(face.replace(url, cache[url]))
    style = "<style>\n" + "\n".join(out) + "\n</style>"
    html = html.replace('<link rel="preconnect" href="https://fonts.googleapis.com">\n', "")
    html = html.replace(m.group(0), style)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "index.html"), "w", encoding="utf-8") as f:
        f.write(html)
    print(f"OK: {len(out)} шрифтів, {len(html)//1024} КБ -> {OUT}/index.html")

if __name__ == "__main__":
    main()
