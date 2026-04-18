# Hex JPEG extractor — reads a serial log file and writes the JPEG payload
# between "JPEG_BEGIN size=N" and "JPEG_END" lines into a .jpg file.
#
# Usage:
#   .\hex_to_jpg.ps1 -InputFile capture.log -OutFile selftest.jpg
#
param(
	[Parameter(Mandatory=$true)] [string] $InputFile,
	[Parameter(Mandatory=$true)] [string] $OutFile
)

$lines = Get-Content $InputFile
$inBlock = $false
$expected = -1
$hex = ""
foreach ($ln in $lines) {
	if ($ln -match "^JPEG_BEGIN\s+size=(\d+)") {
		$expected = [int]$matches[1]
		$inBlock  = $true
		$hex      = ""
		continue
	}
	if ($ln -match "^JPEG_END") {
		break
	}
	if ($inBlock) {
		$hex += ($ln.Trim() -replace "[^0-9a-fA-F]", "")
	}
}

if (-not $inBlock -or $hex.Length -eq 0) {
	Write-Error "No JPEG_BEGIN/JPEG_END block found in $InputFile"
	exit 1
}
if ($hex.Length % 2 -ne 0) {
	Write-Error "Odd number of hex chars ($($hex.Length)); log likely truncated"
	exit 1
}
$bytes = New-Object byte[] ($hex.Length / 2)
for ($i = 0; $i -lt $bytes.Length; $i++) {
	$bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
}
# Resolve to absolute path so .NET writes where the user expects.
$absOut = [IO.Path]::GetFullPath((Join-Path (Get-Location) $OutFile))
[IO.File]::WriteAllBytes($absOut, $bytes)
Write-Host ("OK: wrote {0} bytes to {1} (expected {2})" -f $bytes.Length, $absOut, $expected)
if ($bytes[0] -ne 0xFF -or $bytes[1] -ne 0xD8) {
	Write-Warning "First bytes are not FF D8 — file may be corrupt"
} else {
	Write-Host "SOI marker OK (FF D8)"
}
$last2 = "{0:X2} {1:X2}" -f $bytes[$bytes.Length-2], $bytes[$bytes.Length-1]
if ($bytes[$bytes.Length-2] -ne 0xFF -or $bytes[$bytes.Length-1] -ne 0xD9) {
	Write-Warning ("Last bytes are {0} — expected FF D9" -f $last2)
} else {
	Write-Host "EOI marker OK (FF D9)"
}
