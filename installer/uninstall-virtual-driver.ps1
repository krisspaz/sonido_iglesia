$ErrorActionPreference = 'SilentlyContinue'
$Drivers = Get-WindowsDriver -Online | Where-Object {
    $_.OriginalFileName -like '*ChurchStreamVirtual.inf'
}
foreach ($Driver in $Drivers) {
    & "$env:SystemRoot\System32\pnputil.exe" /delete-driver $Driver.Driver /uninstall /force | Out-Null
}
