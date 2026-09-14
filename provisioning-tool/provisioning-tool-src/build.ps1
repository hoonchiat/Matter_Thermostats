# Builds ../provisioning-tool.html from prov-template.html + esptool.mjs + qrcode.js +
# the light (C:\mhlight) and button (C:\mhbutton) build binaries. Run after `idf.py
# build` in whichever project changed.
#   powershell -ExecutionPolicy Bypass -File .\build.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out  = Join-Path $here '..\provisioning-tool.html'

# esptool-js: rewrite the single ESM `export{...}` into a globalThis assignment.
$esptool = [IO.File]::ReadAllText((Join-Path $here 'esptool.mjs'))
$esptool = [regex]::Replace($esptool, 'export\{([^}]*)\}', {
  param($m)
  $pairs = $m.Groups[1].Value -split ',' | ForEach-Object { $kv = $_ -split ' as '; "$($kv[1].Trim()):$($kv[0].Trim())" }
  'globalThis.esptool={' + ($pairs -join ',') + '}'
})

# qrcode-generator: UMD that defines the global `qrcode` via a top-level var; inline as-is.
$qrlib = [IO.File]::ReadAllText((Join-Path $here 'qrcode.js'))

function B64($p){ [Convert]::ToBase64String([IO.File]::ReadAllBytes($p)) }

# light + button (esp-matter examples: partition-table @ 0xC000, ota_data @ 0x1D000, app @ 0x20000)
$ltbl = B64 'C:\mhlight\build\bootloader\bootloader.bin'
$ltpt = B64 'C:\mhlight\build\partition_table\partition-table.bin'
$ltot = B64 'C:\mhlight\build\ota_data_initial.bin'
$ltap = B64 'C:\mhlight\build\light.bin'
$bnbl = B64 'C:\mhbutton\build\bootloader\bootloader.bin'
$bnpt = B64 'C:\mhbutton\build\partition_table\partition-table.bin'
$bnot = B64 'C:\mhbutton\build\ota_data_initial.bin'
$bnap = B64 'C:\mhbutton\build\button.bin'
# nrf_temp (nRF52840): the whole app is one UF2 (drag-drop). The per-device identity
# UF2 is generated in-browser; only the shared app image is embedded here.
$nrfuf2 = B64 'C:\mhtemp\nrf_temp-nrf52840-supermini.uf2'

$sb = New-Object System.Text.StringBuilder
[void]$sb.Append('window.FW={')
[void]$sb.Append('light:{parts:[{addr:0,b64:"'  + $ltbl + '"},{addr:49152,b64:"'  + $ltpt + '"},{addr:118784,b64:"' + $ltot + '"},{addr:131072,b64:"' + $ltap + '"}]},')
[void]$sb.Append('button:{parts:[{addr:0,b64:"' + $bnbl + '"},{addr:49152,b64:"' + $bnpt + '"},{addr:118784,b64:"' + $bnot + '"},{addr:131072,b64:"' + $bnap + '"}]},')
[void]$sb.Append('nrf_temp:{uf2:true,uf2name:"nrf_temp-nrf52840-supermini.uf2",uf2b64:"' + $nrfuf2 + '"}')
[void]$sb.Append('};')
$fw = $sb.ToString()

$tpl = [IO.File]::ReadAllText((Join-Path $here 'prov-template.html'))
$tpl = $tpl.Replace('/*__QRLIB__*/', $qrlib).Replace('/*__ESPTOOL__*/', $esptool).Replace('/*__FW__*/', $fw)

[IO.File]::WriteAllText($out, $tpl, (New-Object System.Text.UTF8Encoding($false)))
$fi = Get-Item $out
Write-Output ("OK  {0}  ({1:N2} MB)" -f $fi.FullName, ($fi.Length/1MB))
