<#
.SYNOPSIS
  UniversalProductionTools 로컬 포즈 분석기 실행 환경을 설치하거나 점검한다.
.DESCRIPTION
  1) Python 3.13 찾기
  2) ThirdParty\UPTAnalyzer\.venv 가상환경 만들기(이미 있으면 그대로 사용)
  3) requirements-pose-analyzer.txt 패키지 설치
  4) 포즈 모델(약 155MB, 사용자 폴더 .cache\rtmlib) 미리 받기
  5) ffmpeg 확인(링크 구간 다운로드·Vision 분석·정답 영상 생성에 필요, 포즈 분석 자체에는 불필요)
  -CheckOnly를 주면 아무것도 설치·다운로드하지 않고 상태만 점검한다.
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File Plugins\UniversalProductionTools\Tools\setup_analyzer_env.ps1
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File Plugins\UniversalProductionTools\Tools\setup_analyzer_env.ps1 -CheckOnly
#>
param(
    [switch]$CheckOnly,
    [string]$VenvPath = "",
    [string]$Python = ""
)

# 네이티브 명령(python, pip)의 stderr 경고 때문에 스크립트가 멈추지 않도록 종료 코드로만 판단한다.
$ErrorActionPreference = "Continue"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONIOENCODING = "utf-8"

$ToolsDir = $PSScriptRoot
$ProjectDir = (Resolve-Path (Join-Path $ToolsDir "..\..\..")).Path
if (-not $VenvPath) { $VenvPath = Join-Path $ProjectDir "ThirdParty\UPTAnalyzer\.venv" }
$VenvPython = Join-Path $VenvPath "Scripts\python.exe"
$Requirements = Join-Path $ToolsDir "requirements-pose-analyzer.txt"
$Problems = New-Object System.Collections.Generic.List[string]

function Write-Step([string]$Text) { Write-Host ""; Write-Host "== $Text" -ForegroundColor Cyan }
function Write-Ok([string]$Text) { Write-Host "  [OK] $Text" -ForegroundColor Green }
function Add-Problem([string]$Text) { Write-Host "  [문제] $Text" -ForegroundColor Yellow; $script:Problems.Add($Text) }

function Get-PythonInfo([string]$Exe, [string[]]$PrefixArgs) {
    if (-not (Get-Command $Exe -ErrorAction SilentlyContinue)) { return $null }
    $output = & $Exe @PrefixArgs -c "import sys; print('%d.%d|%s' % (sys.version_info[0], sys.version_info[1], sys.executable))"
    if ($LASTEXITCODE -ne 0 -or -not $output) { return $null }
    return ($output | Select-Object -Last 1)
}

function Find-BasePython {
    if ($Python) { return $Python }
    $candidates = @(
        @{ Exe = "py"; Args = @("-3.13") },
        @{ Exe = "python"; Args = @() },
        @{ Exe = "python3"; Args = @() }
    )
    foreach ($candidate in $candidates) {
        $info = Get-PythonInfo $candidate.Exe $candidate.Args
        if (-not $info) { continue }
        $parts = $info.Split("|")
        if ($parts[0] -eq "3.13") { return $parts[1] }
        Write-Host "  Python $($parts[0]) 발견($($parts[1])) - 3.13이 아니라 건너뜀" -ForegroundColor DarkYellow
    }
    return $null
}

$modeText = if ($CheckOnly) { "점검" } else { "설치" }
Write-Host "UniversalProductionTools 분석 환경 $modeText"
Write-Host "  프로젝트: $ProjectDir"
Write-Host "  가상환경: $VenvPath"

Write-Step "1. Python 가상환경"
if (Test-Path $VenvPython) {
    Write-Ok "가상환경이 있습니다: $VenvPython"
} elseif ($CheckOnly) {
    Add-Problem "가상환경이 없습니다. -CheckOnly 없이 실행해 설치하세요."
} else {
    $basePython = Find-BasePython
    if (-not $basePython) {
        Add-Problem "Python 3.13을 찾지 못했습니다. python.org에서 3.13을 설치하거나 -Python 으로 python.exe 경로를 지정하세요."
    } else {
        Write-Host "  기준 Python: $basePython"
        & $basePython -m venv $VenvPath
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $VenvPython)) { Add-Problem "가상환경 생성에 실패했습니다." } else { Write-Ok "가상환경을 만들었습니다." }
    }
}

if (Test-Path $VenvPython) {
    if (-not $CheckOnly) {
        Write-Step "2. 패키지 설치"
        & $VenvPython -m pip install --disable-pip-version-check -r $Requirements
        if ($LASTEXITCODE -ne 0) { Add-Problem "패키지 설치에 실패했습니다(인터넷 연결·프록시를 확인하세요)." } else { Write-Ok "requirements 설치 완료" }
    }

    Write-Step "3. 패키지·포즈 모델·ffmpeg 점검"
    $checkArgs = @((Join-Path $ToolsDir "upt_env_check.py"))
    if (-not $CheckOnly) { $checkArgs += "--download-models" }
    & $VenvPython @checkArgs
    if ($LASTEXITCODE -ne 0) { Add-Problem "점검에서 문제가 발견됐습니다(위 [문제] 줄 참고)." }
}

Write-Step "결과"
if ($Problems.Count -eq 0) {
    Write-Ok "분석 환경이 준비됐습니다."
    Write-Host "  에디터 설정(Editor Preferences > Universal Production Tools > Pose Analyzer)의 Analyzer Python Executable 기본값"
    Write-Host "  'ThirdParty/UPTAnalyzer/.venv/Scripts/python.exe'가 이 가상환경을 가리킵니다."
    Write-Host "  회귀 검사: $VenvPython Plugins\UniversalProductionTools\Tools\eval\run_regression.py"
    exit 0
}
Write-Host "  해결할 문제 $($Problems.Count)건:" -ForegroundColor Yellow
foreach ($problem in $Problems) { Write-Host "   - $problem" -ForegroundColor Yellow }
exit 1
