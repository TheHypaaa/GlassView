# Removes GlassView's per-user install, file associations, shortcut and tessellation cache.
$classes = 'HKCU:\Software\Classes'
$progId = 'GlassView.Model'
$exts = '.stp','.step','.igs','.iges','.brep','.stl','.obj','.ply','.3mf','.glb','.gltf','.gva'
foreach ($e in $exts) { Remove-ItemProperty -Path "$classes\$e\OpenWithProgids" -Name $progId -ErrorAction SilentlyContinue }
Remove-Item -Recurse -Force "$classes\$progId", "$classes\Applications\GlassView.exe" -ErrorAction SilentlyContinue
Remove-Item -Force (Join-Path ([Environment]::GetFolderPath('Programs')) 'GlassView.lnk') -ErrorAction SilentlyContinue
$dll = Join-Path $env:LOCALAPPDATA 'Programs\GlassView\GlassViewPreview.dll'
if (Test-Path $dll) { Start-Process regsvr32 -ArgumentList '/s','/u',"`"$dll`"" -Wait }
Get-Process GlassView, gvkernel, prevhost -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item -Recurse -Force (Join-Path $env:LOCALAPPDATA 'Programs\GlassView') -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force (Join-Path $env:LOCALAPPDATA 'GlassView') -ErrorAction SilentlyContinue
Write-Host 'GlassView removed.' -ForegroundColor Green
