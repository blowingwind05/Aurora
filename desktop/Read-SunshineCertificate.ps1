$ErrorActionPreference = 'Stop'
$connection = [IO.Pipes.NamedPipeClientStream]::new('.', '__PIPE_NAME__', [IO.Pipes.PipeDirection]::Out)
try {
    $connection.Connect(3000)
    $writer = [IO.StreamWriter]::new($connection)
    try {
        $service = Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\SunshineService'
        $image = [regex]::Match($service.ImagePath, '^(?:"([^"]+)"|([^"\r\n]+?\.exe))(?:\s|$)', 'IgnoreCase')
        if (-not $image.Success) { throw 'Service executable not found.' }
        $executable = [Environment]::ExpandEnvironmentVariables(($image.Groups[1].Value + $image.Groups[2].Value))
        if ($executable -notmatch '^[a-zA-Z]:\\' -or [IO.Path]::GetFileName($executable) -ine 'sunshinesvc.exe') { throw 'Invalid service executable.' }
        $directory = Join-Path (Split-Path -Parent (Split-Path -Parent $executable)) 'config'
        $certificate = Join-Path $directory 'credentials\cacert.pem'
        if ($certificate -notmatch '^[a-zA-Z]:\\[^:]+$') { throw 'Certificate is not local.' }
        $item = Get-Item -LiteralPath $certificate -Force
        if ($item.PSIsContainer -or $item.Length -gt 65536) { throw 'Invalid certificate file.' }
        $entry = $item
        while ($null -ne $entry) {
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Certificate path cannot contain links.' }
            $entry = if ($entry -is [IO.FileInfo]) { $entry.Directory } else { $entry.Parent }
        }
        $pem = [IO.File]::ReadAllText($certificate)
        $publicPart = [regex]::Match($pem, '-----BEGIN CERTIFICATE-----\s*([A-Za-z0-9+/=\s]+?)-----END CERTIFICATE-----')
        if (-not $publicPart.Success) { throw 'No public certificate.' }
        $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new([Convert]::FromBase64String($publicPart.Groups[1].Value))
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $writer.Write([BitConverter]::ToString($sha.ComputeHash($cert.RawData)).Replace('-', '')) }
        finally { $sha.Dispose(); $cert.Dispose() }
    }
    finally { $writer.Dispose() }
}
finally { $connection.Dispose() }
