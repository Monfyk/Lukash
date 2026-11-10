#!/usr/bin/env bash
# Збирає офлайн-версії Акордника: dist/Akordnyk.exe (Windows) і dist/Akordnyk.apk (Android).
# Потрібно: python3 + Pillow, Go 1.21+, JDK 17+. Інтернет потрібен лише для першого запуску
# (шрифти Google Fonts, Go-модулі, dx/apksig з Maven Central).
set -euo pipefail
cd "$(dirname "$0")/.."

python3 packaging/offline.py

# Windows: Go + WebView2, іконка та маніфест вшиваються через rsrc_windows_amd64.syso
mkdir -p packaging/windows/www
cp packaging/out/www/index.html packaging/windows/www/index.html
python3 packaging/windows/mksyso.py
(cd packaging/windows && GOOS=windows GOARCH=amd64 CGO_ENABLED=0 \
  go build -trimpath -ldflags "-H windowsgui -s -w" -o ../out/Akordnyk.exe .)

# Android: javac + dx + власні AndroidManifest/resources.arsc + підпис apksig
python3 packaging/android/build_apk.py

mkdir -p dist
cp packaging/out/Akordnyk.exe packaging/out/Akordnyk.apk dist/
ls -lh dist
