# Rebuilds ../flash-tool.html from flash-template.html + esptool.mjs + the current build
# binaries for all 4 firmwares: hub (C:\mho), portal (C:\mhs3), light (C:\mhlight),
# button (C:\mhbutton). Run after `idf.py build` in whichever projects changed.
#   powershell -ExecutionPolicy Bypass -File .\build.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out  = Join-Path $here '..\flash-tool.html'

# esptool-js: rewrite the single ESM `export{A as B,...}` into a globalThis assignment
# so the bundle runs as a plain classic <script> (loads from file:// with no CORS).
$esptool = [IO.File]::ReadAllText((Join-Path $here 'esptool.mjs'))
$esptool = [regex]::Replace($esptool, 'export\{([^}]*)\}', {
  param($m)
  $pairs = $m.Groups[1].Value -split ',' | ForEach-Object {
    $kv = $_ -split ' as '; "$($kv[1].Trim()):$($kv[0].Trim())"
  }
  'globalThis.esptool={' + ($pairs -join ',') + '}'
})

function B64($p){ [Convert]::ToBase64String([IO.File]::ReadAllBytes($p)) }

$c6bl = B64 'C:\mho\build\bootloader\bootloader.bin'
$c6pt = B64 'C:\mho\build\partition_table\partition-table.bin'
$c6ap = B64 'C:\mho\build\matter_hub.bin'
$s3bl = B64 'C:\mhs3\build\bootloader\bootloader.bin'
$s3pt = B64 'C:\mhs3\build\partition_table\partition-table.bin'
$s3ap = B64 'C:\mhs3\build\mh_s3_portal.bin'
# light + button (esp-matter examples: partition-table @ 0xC000, ota_data @ 0x1D000, app @ 0x20000)
$ltbl = B64 'C:\mhlight\build\bootloader\bootloader.bin'
$ltpt = B64 'C:\mhlight\build\partition_table\partition-table.bin'
$ltot = B64 'C:\mhlight\build\ota_data_initial.bin'
$ltap = B64 'C:\mhlight\build\light.bin'
$bnbl = B64 'C:\mhbutton\build\bootloader\bootloader.bin'
$bnpt = B64 'C:\mhbutton\build\partition_table\partition-table.bin'
$bnot = B64 'C:\mhbutton\build\ota_data_initial.bin'
$bnap = B64 'C:\mhbutton\build\button.bin'

# --dirty flags a build with uncommitted changes (the embedded .bin is newer than the commit)
try { $c6ver = (git -C C:\mho      describe --always --dirty).Trim() } catch { $c6ver = 'local' }
try { $s3ver = (git -C C:\mhs3     describe --always --dirty).Trim() } catch { $s3ver = 'local' }
try { $ltver = (git -C C:\mhlight  describe --always --dirty).Trim() } catch { $ltver = 'local' }
try { $bnver = (git -C C:\mhbutton describe --always --dirty).Trim() } catch { $bnver = 'local' }

$sb = New-Object System.Text.StringBuilder
[void]$sb.Append('window.FW={')
[void]$sb.Append('c6:{chip:"ESP32-C6",label:"Matter Hub \u2014 C6",desc:"Thread/Matter hub + schedule &amp; logic engine (build ' + $c6ver + ')",parts:[')
[void]$sb.Append('{addr:0,b64:"' + $c6bl + '"},{addr:32768,b64:"' + $c6pt + '"},{addr:65536,b64:"' + $c6ap + '"}]},')
[void]$sb.Append('s3:{chip:"ESP32-S3",label:"Gateway Portal \u2014 S3",desc:"Browser control panel, bridged to the C6 (build ' + $s3ver + ')",parts:[')
[void]$sb.Append('{addr:0,b64:"' + $s3bl + '"},{addr:32768,b64:"' + $s3pt + '"},{addr:65536,b64:"' + $s3ap + '"}]},')
[void]$sb.Append('light:{chip:"ESP32-C6",label:"RGB Light \u2014 C6",desc:"Matter Extended Color Light accessory - RGB + brightness (build ' + $ltver + ')",parts:[')
[void]$sb.Append('{addr:0,b64:"' + $ltbl + '"},{addr:49152,b64:"' + $ltpt + '"},{addr:118784,b64:"' + $ltot + '"},{addr:131072,b64:"' + $ltap + '"}]},')
[void]$sb.Append('button:{chip:"ESP32-C6",label:"Button \u2014 C6",desc:"Matter Generic Switch accessory - single/double/long press (build ' + $bnver + ')",parts:[')
[void]$sb.Append('{addr:0,b64:"' + $bnbl + '"},{addr:49152,b64:"' + $bnpt + '"},{addr:118784,b64:"' + $bnot + '"},{addr:131072,b64:"' + $bnap + '"}]}')
[void]$sb.Append('};')
$fw = $sb.ToString()

$tpl = [IO.File]::ReadAllText((Join-Path $here 'flash-template.html'))
$tpl = $tpl.Replace('/*__ESPTOOL__*/', $esptool).Replace('/*__FW__*/', $fw)

[IO.File]::WriteAllText($out, $tpl, (New-Object System.Text.UTF8Encoding($false)))
$fi = Get-Item $out
Write-Output ("OK  {0}  ({1:N2} MB)  hub={2} s3={3} light={4} button={5}" -f $fi.FullName, ($fi.Length/1MB), $c6ver, $s3ver, $ltver, $bnver)
