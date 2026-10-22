# 새 버전 릴리스: 저장소의 버전(netmeter.rc·manifest)을 올리고 로컬 빌드로 확인한 뒤, 커밋·태그·push.
# 태그를 받은 GitHub Actions가 같은 소스로 빌드해 릴리스를 만든다 → 로컬 빌드와 배포본 버전이 항상 같음.
# 사용: powershell -ExecutionPolicy Bypass -File release.ps1 1.0.2
param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw "버전은 X.Y.Z 형식이어야 합니다: $Version" }
$comma = "$($Matches[1]),$($Matches[2]),$($Matches[3]),0"
$dot = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
$tag = "v$Version"

if ((git rev-parse --abbrev-ref HEAD) -ne 'main') { throw 'main 브랜치에서 실행하세요.' }
if (git status --porcelain) { throw '커밋하지 않은 변경이 있습니다. 먼저 커밋하세요.' }
if (git tag --list $tag) { throw "이미 있는 태그입니다: $tag" }

# 1) 버전 올리기 (줄바꿈은 그대로 둠)
$rc = 'src\netmeter.rc'
$t = [IO.File]::ReadAllText($rc)
$t = $t -replace 'FILEVERSION\s+[\d,]+', "FILEVERSION $comma" -replace 'PRODUCTVERSION\s+[\d,]+', "PRODUCTVERSION $comma"
$t = $t -replace '"FileVersion", "[^"]*"', "`"FileVersion`", `"$dot`"" -replace '"ProductVersion", "[^"]*"', "`"ProductVersion`", `"$dot`""
[IO.File]::WriteAllText($rc, $t)
$mf = 'src\netmeter.manifest'
$m = [IO.File]::ReadAllText($mf) -replace '(<assemblyIdentity version=")[^"]*"', "`${1}$dot`""
[IO.File]::WriteAllText($mf, $m)

# 2) 로컬 빌드로 확인
cmd /c build.bat
if ($LASTEXITCODE -ne 0) { throw '빌드 실패: 버전 변경은 커밋하지 않았습니다.' }
$built = (Get-Item NetShutter.exe).VersionInfo.ProductVersion
if ($built -ne $dot) { throw "빌드된 exe 버전($built)이 $dot 와 다릅니다." }

# 3) 커밋·태그·push → Actions가 릴리스
git add $rc $mf
git commit -m "버전 $Version"
git tag -a $tag -m "NetShutter $tag"
git push origin main
git push origin $tag
Write-Host "완료: $tag push. 릴리스 진행 상황: https://github.com/ParksoftME/Netshutter/actions"
