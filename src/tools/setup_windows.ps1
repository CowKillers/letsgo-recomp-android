# Preparação única de uma máquina Windows para compilar o Let's Go Recomp (docs/COMPILAR-WINDOWS.md).
# Instala o MSYS2 (clang, lld, make, SDL3, FFmpeg, Vulkan, shaderc) e o Python 3.12, cria o .venv do gerador
# (capstone, unicorn, lz4) e deixa o clone com fim de linha LF. Pode rodar de novo sem problema.
# Adaptado de tools/setup_windows.ps1 do LAS Recomp (sem o randomizer).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path C:\msys64\usr\bin\pacman.exe)) {
    winget install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements --silent
}
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
$bash = 'C:\msys64\usr\bin\bash.exe'
& $bash -lc 'pacman -Syuu --noconfirm' | Out-Null
& $bash -lc 'pacman -Syuu --noconfirm' | Out-Null  # segunda passada depois de uma atualização do núcleo
$pkgs = 'make', 'mingw-w64-ucrt-x86_64-clang', 'mingw-w64-ucrt-x86_64-lld', 'mingw-w64-ucrt-x86_64-sdl3',
        'mingw-w64-ucrt-x86_64-ffmpeg', 'mingw-w64-ucrt-x86_64-vulkan-devel', 'mingw-w64-ucrt-x86_64-shaderc',
        'mingw-w64-ucrt-x86_64-spirv-tools', 'mingw-w64-ucrt-x86_64-pkgconf', 'mingw-w64-ucrt-x86_64-gdb',
        'mingw-w64-ucrt-x86_64-vulkan-memory-allocator', 'mingw-w64-ucrt-x86_64-vulkan-validation-layers'
& $bash -lc "pacman -S --needed --noconfirm $($pkgs -join ' ')"

# Python do python.org (tem os wheels prontos do unicorn / capstone; o Python do MSYS2 não consegue compilá-los)
$py = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
if (-not (Test-Path $py)) {
    winget install --id Python.Python.3.12 -e --scope user --accept-source-agreements --accept-package-agreements --silent
}
if (-not (Test-Path "$root\.venv\Scripts\python.exe")) { & $py -m venv "$root\.venv" }
& "$root\.venv\Scripts\python.exe" -m pip install -q lz4 capstone unicorn

# Fim de linha LF neste clone (os scripts bash quebram com CRLF; o Git for Windows usa autocrlf=true por padrão)
git -C $root config core.autocrlf false
git -C $root config core.eol lf
Write-Host 'Pronto. Compile num shell MSYS2 UCRT64 (ou no Git Bash com C:\msys64\ucrt64\bin;C:\msys64\usr\bin no PATH): make -j20'
