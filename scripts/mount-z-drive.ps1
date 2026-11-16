param(
    [string]$DriveLetter = "Z",
    [string]$DavUrl = "http://127.0.0.1:4173/dav"
)

$ErrorActionPreference = "Stop"
$Drive = "$($DriveLetter.ToUpper()):"

Write-Host "=== ChainIt Sibling Z:\ Drive Mounter ===" -ForegroundColor Cyan

# 1. Ensure Windows built-in WebClient (WebDAV redirector) service is running
$webClient = Get-Service -Name "WebClient" -ErrorAction SilentlyContinue
if ($null -eq $webClient) {
    Write-Warning "Windows WebClient service not found. Install WebDAV Redirector or use ChainIt WinFsp mode."
} elseif ($webClient.Status -ne "Running") {
    Write-Host "Starting Windows WebClient service..." -ForegroundColor Yellow
    try {
        Start-Service -Name "WebClient"
    } catch {
        Write-Warning "Run PowerShell as Administrator once to start the WebClient service, or set it to Automatic."
    }
}

# 2. If running as Administrator, bump Windows WebClient default 50 MB limit to 4 GB (0xFFFFFFFF)
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($isAdmin) {
    $regPath = "HKLM:\SYSTEM\CurrentControlSet\Services\WebClient\Parameters"
    Set-ItemProperty -Path $regPath -Name "FileSizeLimitInBytes" -Value 0xffffffff -Type DWord
    Write-Host "Configured WebClient FileSizeLimitInBytes = 4 GB." -ForegroundColor Green
}

# 3. Clean up any existing mapping on Z:
if (Test-Path "$Drive\") {
    Write-Host "Refreshing existing mapping on $Drive..." -ForegroundColor DarkGray
    net use $Drive /delete /y | Out-Null
}

# 4. Map ChainIt Sibling Phone VFS to Z:
Write-Host "Mounting Sibling Phone storage at $Drive -> $DavUrl ..." -ForegroundColor Cyan
net use $Drive $DavUrl /persistent:no

if ($LASTEXITCODE -eq 0) {
    # Set friendly label in Windows File Explorer
    try {
        $shell = New-Object -ComObject Shell.Application
        $folder = $shell.NameSpace("$Drive\")
        if ($null -ne $folder) {
            $folder.Self.Name = "Galaxy S24 [ChainIt Sibling]"
        }
    } catch {}

    Write-Host "SUCCESS! Sibling Phone mounted at $Drive\ (Zero-touch VFS active)." -ForegroundColor Green
    Start-Process "explorer.exe" "$Drive\"
} else {
    Write-Error "Failed to mount $Drive. Make sure ChainIt Transfer Engine is running on $DavUrl."
}
