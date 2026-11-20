#Requires -Version 7.3

param(
    [string] $AssemblyPath = (Join-Path $PSScriptRoot '../dist/Avalonia-0.5.1/Aurora.dll'),
    [switch] $CheckInstalledSunshine,
    [switch] $CheckElevatedImport
)

$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $AssemblyPath).Path)
$storeType = $assembly.GetType('Aurora.SunshineCredentialStore', $true)
$adminType = $assembly.GetType('Aurora.SunshineAdmin', $true)

function Assert-True {
    param([bool] $Condition, [string] $Reason)
    if (-not $Condition) { throw $Reason }
}

$scope = 'test-' + [Guid]::NewGuid().ToString('N')
$store = [Activator]::CreateInstance($storeType, @($scope))
$other = [Activator]::CreateInstance($storeType, @("$scope-other"))
$password = [Guid]::NewGuid().ToString('N') + ':密码'
try {
    Assert-True ($null -eq $store.Read()) 'A new entry must not have stored credentials.'
    $store.Save('test-user', $password)
    $saved = $store.Read()
    Assert-True ($saved.Username -ceq 'test-user' -and $saved.Password -ceq $password) 'WinCred must return saved Unicode login.'
    Assert-True ($null -eq $other.Read()) 'A different endpoint scope must not reuse the entry.'
}
finally { $store.Forget() }
Assert-True ($null -eq $store.Read()) 'Forget must remove persisted credentials.'

$key = [Security.Cryptography.RSA]::Create(2048)
$request = [Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=test',
    $key, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
$certificate = $request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-1), [DateTimeOffset]::UtcNow.AddMinutes(5))
try {
    $verify = $adminType.GetMethod('MatchesLocalCertificate', [Reflection.BindingFlags]'Static,NonPublic')
    $endpoint = [uri]'https://127.0.0.1:47990/'
    $hash = $certificate.GetCertHash([Security.Cryptography.HashAlgorithmName]::SHA256)
    Assert-True ($verify.Invoke($null, @($endpoint, $endpoint, $hash, $certificate))) 'Matching local server certificate should pass.'
    Assert-True (-not $verify.Invoke($null, @($endpoint, [uri]'https://127.0.0.1:47991/', $hash, $certificate))) 'Credentials must not be sent to another local port.'
    Assert-True (-not $verify.Invoke($null, @($endpoint, [uri]'https://example.test/', $hash, $certificate))) 'A remote endpoint must not pass the certificate check.'
    $hash[0] = $hash[0] -bxor 1
    Assert-True (-not $verify.Invoke($null, @($endpoint, $endpoint, $hash, $certificate))) 'An unmatched certificate must not pass.'
}
finally { $certificate.Dispose(); $key.Dispose() }

if ($CheckInstalledSunshine) {
    $admin = $adminType.GetMethod('OpenLocal', [Type[]]@()).Invoke($null, $null)
    try { "Installed Sunshine read-only HTTPS check: $($admin.CheckAccess().GetAwaiter().GetResult())" }
    finally { $admin.Dispose() }
}

if ($CheckElevatedImport) {
    $trust = $assembly.GetType('Aurora.SunshineTrust', $true)
    $fingerprint = $trust.GetMethod('ReadElevated').Invoke($null, $null).GetAwaiter().GetResult()
    $admin = $adminType.GetMethod('OpenLocal', [Type[]]@([byte[]])).Invoke($null, @(,$fingerprint))
    try { "Elevated import read-only HTTPS check: $($admin.CheckAccess().GetAwaiter().GetResult())" }
    finally { $admin.Dispose() }
}

'Sunshine credential storage and certificate pinning tests passed.'
