#!/usr/bin/env bash
# Build CompactMdViewer (Win32 + WebView2 + md4c) with MSVC 2022, from Git Bash.
# Requires: Visual Studio 2022 (C++ workload) and WebView2 Runtime.
# The exe is built to build/MdViewer.exe. If the deploy folder
# F:\Run\Textutils\mdviewer exists, the exe is also copied there.
set -e
cd "$(dirname "$0")"

VCVARSALL='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat'
mkdir -p build

# Run the whole build inside one cmd session so the vcvarsall
# environment applies to rc and cl.
cmd //c "call \"$VCVARSALL\" x64 >nul && rc -nologo -fo build\app.res src\app.rc && cl -nologo -W4 -EHsc -std:c++17 -O2 -MT -utf-8 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -I third_party\wv2\build\native\include -I third_party\md4c\md4c-release-0.5.3\src src\main.cpp third_party\md4c\md4c-release-0.5.3\src\md4c.c third_party\md4c\md4c-release-0.5.3\src\md4c-html.c third_party\md4c\md4c-release-0.5.3\src\entity.c build\app.res -Fo:build\ -Fe:build\MdViewer.exe -link -SUBSYSTEM:WINDOWS third_party\wv2\build\native\x64\WebView2LoaderStatic.lib shlwapi.lib shell32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib advapi32.lib comdlg32.lib dwmapi.lib"

if [ -d /f/Run/Textutils/mdviewer ]; then
  cp build/MdViewer.exe /f/Run/Textutils/mdviewer/
  echo "OK: F:\\Run\\Textutils\\mdviewer\\MdViewer.exe"
else
  echo "OK: build\\MdViewer.exe (deploy folder not found, skipped)"
fi
