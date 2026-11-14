
$ProjectPath = "C:\Users\ADMIN\Gateway-device"
$SourcePath = Join-Path $ProjectPath "main"
$ReportPath = Join-Path $ProjectPath "clang_report.txt"

if (!(Get-Command clang -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: Clang is not installed or not in PATH."
    exit 1
}

if (!(Test-Path $SourcePath)) {
    Write-Host "ERROR: main folder not found."
    exit 1
}

"Clang Static Analysis Report" | Out-File $ReportPath
"Project: $ProjectPath" | Out-File $ReportPath -Append
"Date: $(Get-Date)" | Out-File $ReportPath -Append
"`n" | Out-File $ReportPath -Append

$Files = Get-ChildItem $SourcePath -Filter *.c -Recurse

foreach ($File in $Files) {
    "`n========================================" |
        Out-File $ReportPath -Append
    "File: $($File.FullName)" |
        Out-File $ReportPath -Append
    "========================================" |
        Out-File $ReportPath -Append

    $Output = & clang --analyze -Wno-everything `
        $File.FullName 2>&1

    if ($Output) {
        $Output | Out-File $ReportPath -Append
    } else {
        "No diagnostics reported." |
            Out-File $ReportPath -Append
    }
}

Write-Host "Analysis completed."
Write-Host "Report saved to: $ReportPath"
