param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Thumbprint
)

$ErrorActionPreference = 'Stop'
# A caller running PowerShell 7 may pass its incompatible module path to powershell.exe.
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1"
$signature = Get-AuthenticodeSignature -LiteralPath $Path
if (-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $Thumbprint) {
    throw "Unexpected or missing signing certificate: $Path"
}
if (-not $signature.TimeStamperCertificate) {
    throw "Missing online timestamp: $Path"
}
# Windows reports CERT_E_UNTRUSTEDROOT as UnknownError on some PowerShell versions.
# Match that exact system error, never a general UnknownError or trust failure.
$rootError = New-Object System.ComponentModel.Win32Exception(-2146762487)
$selfSignedRoot = ($signature.Status -eq 'NotTrusted' -or $signature.Status -eq 'UnknownError') -and
    $signature.StatusMessage.Trim().TrimEnd('.') -eq $rootError.Message.Trim().TrimEnd('.') -and
    $signature.SignerCertificate.Subject -eq $signature.SignerCertificate.Issuer
if ($signature.Status -ne 'Valid' -and -not $selfSignedRoot) {
    throw "Invalid Authenticode signature ($($signature.Status)): $($signature.StatusMessage)"
}

# A self-signed publisher is expected; an untrusted timestamp authority is not.
$timestampChain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
try {
    if (-not $timestampChain.Build($signature.TimeStamperCertificate)) {
        throw "Untrusted timestamp authority: $($timestampChain.ChainStatus.StatusInformation -join '; ')"
    }
} finally {
    $timestampChain.Dispose()
}

if ($selfSignedRoot) {
    Write-Host 'NOTE: self-signed publisher; Windows and SmartScreen may still warn.'
}
Write-Host "Verified publisher, file integrity and online timestamp: $Path"
