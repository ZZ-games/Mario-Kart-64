# Mario Kart 64 Recompiled - local web server (PowerShell, nothing to install).
# Listens on 127.0.0.1 only. Sends the cross-origin isolation headers the game's
# threads need (SharedArrayBuffer). Close this window to stop it.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Test-Path (Join-Path $root 'mk64.z64'))) {
  Write-Host "mk64.z64 is missing. Copy your Mario Kart 64 (USA) ROM into:" -ForegroundColor Yellow
  Write-Host "  $root" ; Write-Host "and name it mk64.z64, then run play.bat again." ; Read-Host "Press Enter to close"; exit 1
}
$port = 8643
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://127.0.0.1:$port/")
try { $listener.Start() } catch { Write-Host "Port $port is busy. Is play.bat already running?" -ForegroundColor Yellow; Read-Host "Press Enter to close"; exit 1 }
$url = "http://127.0.0.1:$port/index.html"
$types = @{ '.html'='text/html; charset=utf-8'; '.js'='text/javascript; charset=utf-8'; '.wasm'='application/wasm'; '.z64'='application/octet-stream' }
$brave = @("$env:ProgramFiles\BraveSoftware\Brave-Browser\Application\brave.exe",
           "${env:ProgramFiles(x86)}\BraveSoftware\Brave-Browser\Application\brave.exe",
           "$env:LOCALAPPDATA\BraveSoftware\Brave-Browser\Application\brave.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($brave) { Start-Process $brave $url } else { Start-Process $url }
Write-Host "Mario Kart 64 is running at $url" -ForegroundColor Green
Write-Host "Close this window when you're done playing."
while ($listener.IsListening) {
  $ctx = $listener.GetContext()
  $path = [Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath.TrimStart('/'))
  if ($path -eq '') { $path = 'index.html' }
  $file = [IO.Path]::GetFullPath((Join-Path $root $path))
  $res = $ctx.Response
  $res.Headers.Add('Cross-Origin-Opener-Policy', 'same-origin')
  $res.Headers.Add('Cross-Origin-Embedder-Policy', 'require-corp')
  $res.Headers.Add('Cache-Control', 'no-store')
  if ($file.StartsWith($root) -and (Test-Path $file -PathType Leaf)) {
    $bytes = [IO.File]::ReadAllBytes($file)
    $ext = [IO.Path]::GetExtension($file).ToLower()
    $res.ContentType = if ($types[$ext]) { $types[$ext] } else { 'application/octet-stream' }
    $res.ContentLength64 = $bytes.Length
    try { $res.OutputStream.Write($bytes, 0, $bytes.Length) } catch {}
  } else { $res.StatusCode = 404 }
  $res.Close()
}
