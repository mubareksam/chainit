param(
    [string]$DriveLetter = "Z"
)

$Drive = "$($DriveLetter.ToUpper()):"
Write-Host "Unmounting ChainIt Sibling drive $Drive ..." -ForegroundColor Yellow
net use $Drive /delete /y
Write-Host "Unmounted $Drive." -ForegroundColor Green
