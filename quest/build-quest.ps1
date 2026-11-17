# Builds the Meta Quest (standalone) version of Super Hang-On Ultimate into SuperHangOnQuest.apk,
# with your ROMs inside it, and installs it on a headset if one is connected by USB (otherwise sideload the APK
# your usual way).
#
#   Right-click > Run with PowerShell, or from a PowerShell window in this folder:
#       powershell -ExecutionPolicy Bypass -File .\build-quest.ps1
#
# The first run downloads what it needs into the "tools" folder next to this script (about 2.5 GB:
# Java, Google's Android command-line tools and NDK, the OpenXR loader). Nothing is installed
# system-wide; delete the tools folder to remove it all. Google's licences are shown for you to accept.
#
# Options:
#   -NoInstall     build the APK only
#   -Clean         compile everything again
param([switch]$NoInstall, [switch]$Clean)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'      # (downloads are many times faster without the progress bar)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$src   = Join-Path $here '..\source'
$tools = Join-Path $here 'tools'
$build = Join-Path $here 'build'
$sdk   = Join-Path $tools 'sdk'
$pkg   = 'com.shangon.ultimate'
$ndkVersion   = '27.3.13750724'
$buildTools   = '34.0.0'
$platform     = 'android-34'
$loaderVer    = '1.1.47'
New-Item -ItemType Directory -Force $tools, $build | Out-Null

function Say($t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }
function Fail($t) { Write-Host ""; Write-Host "FAILED: $t" -ForegroundColor Red; Write-Host "Send the text above to get help."; Read-Host "Press Enter to close"; exit 1 }
function Fetch($url, $out) {
    if (Test-Path $out) { return }
    Write-Host "   downloading $url"
    try { Invoke-WebRequest -Uri $url -OutFile "$out.part" -UseBasicParsing } catch { Fail "could not download $url : $_" }
    Move-Item -Force "$out.part" $out
}
function Unzip($zip, $dest) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $dest)
}

if (-not (Test-Path (Join-Path $src 'src\gen\main_rc.c'))) { Fail "the game code made from your ROM (source\src\gen) is missing. In the project folder, run first:  python setup.py --rom path\to\shangon.zip" }

# ---- 1. Java (only used by Google's tools) ----
Say "Java"
$jdk = Join-Path $tools 'jdk'
if (-not (Test-Path (Join-Path $jdk 'bin\java.exe'))) {
    $zip = Join-Path $tools 'jdk.zip'
    Fetch 'https://api.adoptium.net/v3/binary/latest/17/ga/windows/x64/jdk/hotspot/normal/eclipse' $zip
    $tmp = Join-Path $tools 'jdk_tmp'; Unzip $zip $tmp
    $inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
    if (Test-Path $jdk) { Remove-Item -Recurse -Force $jdk }
    Move-Item $inner.FullName $jdk; Remove-Item -Recurse -Force $tmp
}
$env:JAVA_HOME = $jdk; $env:PATH = "$jdk\bin;$env:PATH"
Write-Host "   $jdk"

# ---- 2. Android command-line tools, platform, build tools, NDK ----
Say "Android tools"
$sdkmanager = Join-Path $sdk 'cmdline-tools\latest\bin\sdkmanager.bat'
if (-not (Test-Path $sdkmanager)) {
    $zip = Join-Path $tools 'cmdline-tools.zip'
    Fetch 'https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip' $zip
    $tmp = Join-Path $tools 'clt_tmp'; Unzip $zip $tmp
    New-Item -ItemType Directory -Force (Join-Path $sdk 'cmdline-tools') | Out-Null
    Move-Item (Join-Path $tmp 'cmdline-tools') (Join-Path $sdk 'cmdline-tools\latest'); Remove-Item -Recurse -Force $tmp
}
$ndk = Join-Path $sdk "ndk\$ndkVersion"
$bt  = Join-Path $sdk "build-tools\$buildTools"
$androidJar = Join-Path $sdk "platforms\$platform\android.jar"
$adb = Join-Path $sdk 'platform-tools\adb.exe'
if (-not ((Test-Path $ndk) -and (Test-Path $bt) -and (Test-Path $androidJar) -and (Test-Path $adb))) {
    Write-Host "   Google's licences follow: type y and Enter to accept each (needed once)." -ForegroundColor Yellow
    & $sdkmanager "--sdk_root=$sdk" --licenses
    & $sdkmanager "--sdk_root=$sdk" "platform-tools" "platforms;$platform" "build-tools;$buildTools" "ndk;$ndkVersion"
    if ($LASTEXITCODE -ne 0) { Fail "Google's sdkmanager could not install the Android tools" }
}
foreach ($p in @($ndk, $bt, $androidJar, $adb)) { if (-not (Test-Path $p)) { Fail "missing after install: $p" } }
$tc = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64'
$cc = Join-Path $tc 'bin\clang.exe'

# ---- 3. the OpenXR loader (Khronos, for Android) ----
Say "OpenXR loader"
$loaderDir = Join-Path $tools "openxr_loader_$loaderVer"
$loaderSo = Get-ChildItem $loaderDir -Recurse -Filter 'libopenxr_loader.so' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'arm64-v8a' } | Select-Object -First 1
if (-not $loaderSo) {
    $zip = Join-Path $tools "openxr_loader_$loaderVer.zip"
    Fetch "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$loaderVer/openxr_loader_for_android-$loaderVer.aar" $zip
    Unzip $zip $loaderDir
    $loaderSo = Get-ChildItem $loaderDir -Recurse -Filter 'libopenxr_loader.so' | Where-Object { $_.FullName -match 'arm64-v8a' } | Select-Object -First 1
}
if (-not $loaderSo) { Fail "the OpenXR loader package has no arm64 library" }
Write-Host "   $($loaderSo.FullName)"

# ---- 4. compile ----
Say "Compiling the game (the first time takes a few minutes)"
$obj = Join-Path $build 'obj'
if ($Clean -and (Test-Path $obj)) { Remove-Item -Recurse -Force $obj }
New-Item -ItemType Directory -Force $obj | Out-Null
$glue = Join-Path $ndk 'sources\android\native_app_glue'
$M = 'src\third_party\musashi'
# the hand-written overrides as one file (in a repository checkout they are recomp\overrides\*.c, made by setup.py)
$ovr = Join-Path $build 'overrides_all.c'
$ovrDir = Join-Path $src 'recomp\overrides'
if (Test-Path (Join-Path $src 'src\overrides_all.c')) { Copy-Item -Force (Join-Path $src 'src\overrides_all.c') $ovr }
elseif (Test-Path $ovrDir) {
    $text = (Get-ChildItem (Join-Path $ovrDir '*.c') | Sort-Object Name | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
    if ((-not (Test-Path $ovr)) -or ((Get-Content -LiteralPath $ovr -Raw) -ne $text)) { Set-Content -LiteralPath $ovr -Value $text -NoNewline }
} else { Fail "the overrides made from your ROM (source\recomp\overrides) are missing: run  python setup.py --rom path\to\shangon.zip  first" }
$sources = @('src\quest_main.c','src\ini_file.c','src\launcher_core.c','src\sho_controls.c','src\rc_runtime.c','src\rc_ctx.c','src\gen\main_rc.c','src\gen\sub_rc.c',$ovr,
             'src\sho_core.c','src\sho_video.c','src\sho_video_eye.c','src\sho_music.c','src\sho_md.c','src\sho_mdgfx.c','src\sho_mdcourse.c','src\sho_mdsnd.c',
             "$M\m68kcpu.c","$M\m68kops.c","$M\m68kdasm.c","$M\softfloat\softfloat.c",
             'src\third_party\z80\z80.c','src\third_party\nukedopn2\ym3438.c','src\third_party\nukedopm\opm.c','src\third_party\puff\puff.c')
$cflags = @('--target=aarch64-linux-android29','-O2','-DNDEBUG','-DRC_ASM_FIBERS','-fPIC','-fno-strict-aliasing','-ffp-contract=off','-w','-Isrc','-I.',"-I$glue")
$objs = @()
Push-Location $src
try {
    foreach ($s in $sources) {
        $o = Join-Path $obj (([IO.Path]::GetFileNameWithoutExtension($s)) + '.o')
        $objs += $o
        if ((Test-Path $o) -and ((Get-Item $o).LastWriteTime -gt (Get-Item $s).LastWriteTime)) { continue }
        Write-Host "   $s"
        & $cc @cflags -c $s -o $o
        if ($LASTEXITCODE -ne 0) { Fail "could not compile $s" }
    }
    $o = Join-Path $obj 'android_native_app_glue.o'; $objs += $o
    if (-not (Test-Path $o)) { & $cc @cflags -c (Join-Path $glue 'android_native_app_glue.c') -o $o; if ($LASTEXITCODE -ne 0) { Fail "could not compile the Android glue" } }
} finally { Pop-Location }

$libDir = Join-Path $build 'lib\arm64-v8a'
New-Item -ItemType Directory -Force $libDir | Out-Null
$so = Join-Path $libDir 'libshangon.so'
Write-Host "   linking"
& $cc '--target=aarch64-linux-android29' -shared -o $so @objs '-u' 'ANativeActivity_onCreate' "-L$($loaderSo.DirectoryName)" '-lopenxr_loader' '-landroid' '-llog' '-lEGL' '-lGLESv3' '-laaudio' '-lm' '-Wl,-z,max-page-size=16384' '-Wl,--no-undefined'
if ($LASTEXITCODE -ne 0) { Fail "could not link the game" }
Copy-Item -Force $loaderSo.FullName (Join-Path $libDir 'libopenxr_loader.so')
$cxx = Join-Path $tc 'sysroot\usr\lib\aarch64-linux-android\libc++_shared.so'
if (-not (Test-Path $cxx)) { Fail "the NDK has no libc++_shared.so at $cxx" }
Copy-Item -Force $cxx (Join-Path $libDir 'libc++_shared.so')

# ---- 5. the APK ----
Say "Packing the APK"
$base = Join-Path $build 'base.apk'; $aligned = Join-Path $build 'aligned.apk'; $apk = Join-Path $here 'SuperHangOnQuest.apk'
Remove-Item -Force $base, $aligned -ErrorAction SilentlyContinue
# your ROMs go inside the APK (it is a private build anyway), so nothing has to be copied to the headset by hand:
# shangon.zip and a Mega Drive ROM, if they are next to this script or in the project folder
$assets = Join-Path $build 'assets'
if (Test-Path $assets) { Remove-Item -Recurse -Force $assets }
New-Item -ItemType Directory -Force $assets | Out-Null
$haveRom = $false
foreach ($dir in @($here, (Join-Path $here '..'))) {
    foreach ($f in Get-ChildItem $dir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq 'shangon.zip' -or $_.Extension -in '.md', '.gen', '.bin' -or ($_.Extension -eq '.zip' -and $_.Name -match 'Hang' -and $_.Name -notmatch 'Quest') }) {
        if (-not (Test-Path (Join-Path $assets $f.Name))) { Copy-Item $f.FullName $assets; Write-Host "   including $($f.Name)" }
        if ($f.Name -eq 'shangon.zip') { $haveRom = $true }
    }
}
$bg = Join-Path $src 'res\launcher_bg.png'
if (Test-Path $bg) { Copy-Item $bg $assets } else { Write-Host "   (no res\launcher_bg.png: the launcher will have a plain background)" -ForegroundColor Yellow }
if (-not $haveRom) { Write-Host "   shangon.zip is not next to this script: the APK will be made without it, and the game will ask for it." -ForegroundColor Yellow }
& (Join-Path $bt 'aapt2.exe') link -o $base --manifest (Join-Path $here 'AndroidManifest.xml') -I $androidJar -A $assets
if ($LASTEXITCODE -ne 0) { Fail "aapt2 could not make the package" }
Add-Type -AssemblyName System.IO.Compression; Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipf = [System.IO.Compression.ZipFile]::Open($base, 'Update')
try {
    foreach ($f in Get-ChildItem $libDir -Filter '*.so') {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zipf, $f.FullName, "lib/arm64-v8a/$($f.Name)", [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $zipf.Dispose() }
& (Join-Path $bt 'zipalign.exe') -f 4 $base $aligned
if ($LASTEXITCODE -ne 0) { Fail "zipalign failed" }
$ks = Join-Path $tools 'debug.keystore'
if (-not (Test-Path $ks)) {
    & (Join-Path $jdk 'bin\keytool.exe') -genkeypair -keystore $ks -storepass android -keypass android -alias key -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=Super Hang-On Ultimate (private build)'
    if ($LASTEXITCODE -ne 0) { Fail "could not make a signing key" }
}
& (Join-Path $bt 'apksigner.bat') sign --ks $ks --ks-pass pass:android --key-pass pass:android --out $apk $aligned
if ($LASTEXITCODE -ne 0) { Fail "could not sign the APK" }
Write-Host "   $apk" -ForegroundColor Green

# ---- 6. onto the headset ----
if (-not $NoInstall) {
    Say "Installing on the headset"
    $dev = (& $adb devices) | Where-Object { $_ -match "`tdevice$" }
    if (-not $dev) {
        Write-Host "   No headset is connected by USB, so nothing was installed - that is fine:" -ForegroundColor Yellow
        Write-Host "   sideload SuperHangOnQuest.apk your usual way. Your ROMs are inside it." -ForegroundColor Yellow
    } else {
        & $adb install -r $apk
        if ($LASTEXITCODE -ne 0) { Fail "the headset refused the APK" }
        Write-Host "   Installed. In the headset: Library > filter 'Unknown sources' > Super Hang-On Ultimate." -ForegroundColor Green
    }
}
Write-Host ""
Read-Host "Done. Press Enter to close"
