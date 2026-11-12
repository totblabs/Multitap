# Grades the client's AI-generated scenic images (tools/backgrounds/Grade.cs) so each
# scene is the same place by day and by night, into
# apps/windows_client/assets/backgrounds. The two AI-generated source images stay
# byte-identical; only the derived counterparts are written.
# Deterministic and offline.
param([string]$Output = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$assets = Join-Path $root 'apps/windows_client/assets/backgrounds'
if (-not $Output) { $Output = $assets }
New-Item -ItemType Directory -Force -Path $Output | Out-Null
if (-not ('TotbGrade' -as [type])) {
  Add-Type -TypeDefinition ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'Grade.cs'))) -ReferencedAssemblies System.Drawing
}

# Skylines traced on the 1672x941 sources as x,y pixel pairs.
function Skyline([int[]]$pairs, [int]$bias) {
  $line = New-Object 'float[]' $pairs.Length
  for ($i = 0; $i -lt $pairs.Length; $i += 2) { $line[$i] = $pairs[$i]; $line[$i + 1] = $pairs[$i + 1] + $bias }
  return , $line
}
# Daylight meadow: snowy distant range (kept just under the skyline), then
# high-contrast rock (the colour mask finds the exact edge below a looser line).
$meadow = (Skyline @(0,432, 50,417, 80,435, 125,426, 155,437, 210,412, 230,420, 255,417, 300,447, 310,455, 355,442, 390,438, 420,447, 455,440, 490,431, 522,429, 545,436, 580,437, 600,447, 620,445, 660,450, 688,435, 705,447, 730,438, 760,450, 800,452, 815,448, 835,430, 853,412) -2) +
  (Skyline @(870,415, 903,420, 945,425, 982,362, 1012,371, 1032,300, 1062,257, 1087,262, 1107,300, 1128,271, 1149,237, 1178,211, 1207,250, 1220,242, 1249,227, 1278,254, 1291,242, 1312,175, 1324,171, 1341,158, 1362,162, 1387,125, 1403,96, 1424,85, 1453,104, 1482,129, 1503,123, 1528,150, 1545,183, 1574,192, 1599,158, 1620,146, 1645,167, 1662,187, 1672,200) 0)
# Evening peaks: the sky is replaced up to the line, so it follows the rock.
$peaks = Skyline @(0,490, 40,495, 78,475, 112,467, 150,481, 195,486, 210,475, 245,456, 280,472, 300,469, 320,465, 357,447, 380,455, 397,449, 420,462, 440,456, 455,452, 480,440, 500,425, 530,407, 557,398, 580,407, 600,420, 620,432, 635,432, 647,427, 660,434, 680,429, 705,417, 730,427, 750,422, 770,436, 790,437, 847,412, 865,405, 933,355, 954,334, 976,341, 997,344, 1033,355, 1040,330, 1054,280, 1076,262, 1111,255, 1140,262, 1169,266, 1226,271, 1254,274, 1283,280, 1304,294, 1319,312, 1340,314, 1369,316, 1379,307, 1397,310, 1411,319, 1440,337, 1483,341, 1504,355, 1519,362, 1533,337, 1547,294, 1561,280, 1576,280, 1597,294, 1619,309, 1640,301, 1654,291, 1672,294) 2

$jobs = @(
  @{ Name = 'alpine-dusk-day.jpg'; Run = { param($o) [TotbGrade]::Day((Join-Path $assets 'alpine-dusk.png'), $o, $peaks) } },
  @{ Name = 'alpine-daylight-night.jpg'; Run = { param($o) [TotbGrade]::Night((Join-Path $assets 'alpine-daylight.png'), $o, $meadow, 0.47, 0.13) } }
)
foreach ($job in $jobs) {
  $path = Join-Path $Output $job.Name
  $watch = [Diagnostics.Stopwatch]::StartNew()
  & $job.Run $path
  Write-Output ("{0} {1} KB {2:n1}s" -f $job.Name, [math]::Round((Get-Item $path).Length / 1KB), $watch.Elapsed.TotalSeconds)
}
