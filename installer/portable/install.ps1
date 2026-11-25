# GlassView per-user installer: no admin rights needed.
# Copies GlassView.exe to %LOCALAPPDATA%\Programs\GlassView, adds a Start Menu shortcut,
# and registers GlassView under "Open with" for STEP/IGES/STL/OBJ/PLY/3MF/glTF files.
$ErrorActionPreference = 'Stop'
$src  = Join-Path $PSScriptRoot 'GlassView.exe'
$dest = Join-Path $env:LOCALAPPDATA 'Programs\GlassView'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item $src (Join-Path $dest 'GlassView.exe') -Force
$kernel = Join-Path $PSScriptRoot 'gvkernel.exe'
if (Test-Path $kernel) { Copy-Item $kernel (Join-Path $dest 'gvkernel.exe') -Force }
$opener = Join-Path $PSScriptRoot 'GlassViewOpen.exe'
if (Test-Path $opener) { Copy-Item $opener (Join-Path $dest 'GlassViewOpen.exe') -Force }
$preview = Join-Path $PSScriptRoot 'GlassViewPreview.dll'
if (Test-Path $preview) { Copy-Item $preview (Join-Path $dest 'GlassViewPreview.dll') -Force }
$exe = Join-Path $dest 'GlassView.exe'
# double-click goes through GlassViewOpen.exe, which hands the file to an already open GlassView window
$open = Join-Path $dest 'GlassViewOpen.exe'
if (-not (Test-Path $open)) { $open = $exe }

$classes = 'HKCU:\Software\Classes'
$progId  = 'GlassView.Model'
New-Item -Force "$classes\$progId" | Out-Null
Set-ItemProperty "$classes\$progId" '(default)' '3D model'
New-Item -Force "$classes\$progId\DefaultIcon" | Out-Null
Set-ItemProperty "$classes\$progId\DefaultIcon" '(default)' "`"$exe`",0"
New-Item -Force "$classes\$progId\shell\open\command" | Out-Null
Set-ItemProperty "$classes\$progId\shell\open\command" '(default)' "`"$open`" `"%1`""

$app = "$classes\Applications\GlassView.exe"
New-Item -Force "$app\shell\open\command" | Out-Null
Set-ItemProperty "$app" 'FriendlyAppName' 'GlassView'
Set-ItemProperty "$app\shell\open\command" '(default)' "`"$open`" `"%1`""
New-Item -Force "$app\SupportedTypes" | Out-Null

$exts = '.stp','.step','.igs','.iges','.brep','.stl','.obj','.ply','.3mf','.glb','.gltf','.gva'
foreach ($e in $exts) {
  New-Item -Force "$classes\$e\OpenWithProgids" | Out-Null
  New-ItemProperty -Force -Path "$classes\$e\OpenWithProgids" -Name $progId -PropertyType None -Value ([byte[]]@()) | Out-Null
  New-ItemProperty -Force -Path "$app\SupportedTypes" -Name $e -PropertyType String -Value '' | Out-Null
}

$lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'GlassView.lnk'
$sh = New-Object -ComObject WScript.Shell
$s = $sh.CreateShortcut($lnk); $s.TargetPath = $exe; $s.WorkingDirectory = $dest; $s.Save()

# refresh Explorer's file association cache
Add-Type -Namespace Win32 -Name Shell -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, int f, IntPtr a, IntPtr b);'
[Win32.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)

Write-Host ''
Write-Host "GlassView installed to $dest" -ForegroundColor Green
Write-Host 'Explorer 3D preview: GlassView > Settings > Windows > 3D preview in Explorer.'
Write-Host 'To make it the default: right-click a .stp/.stl file > Open with > Choose another app > GlassView > Always.'
