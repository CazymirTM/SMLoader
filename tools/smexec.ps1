# Run Lua inside the running game through LuaConsole's pipe.
#   .\smexec.ps1 'sm.version'
#   .\smexec.ps1 -Env raw 'ffi ~= nil'
#   .\smexec.ps1 -File ..\scripts\find.lua -Env raw -State 0
param(
    [Parameter(Position = 0)][string]$Code,
    [ValidateSet('game', 'raw')][string]$Env = 'game',
    [ValidateSet('client', 'server', 'none')][string]$Ctx = 'client',
    [int]$State = -1,
    [string]$File,
    [int]$TimeoutMs = 5000
)
if ($File) { $Code = Get-Content -Raw -Encoding utf8 $File }
if (-not $Code) { throw 'No Lua code given.' }

$c = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'SMLoaderConsole', [System.IO.Pipes.PipeDirection]::InOut)
try {
    $c.Connect($TimeoutMs)
    $c.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
    $req = [Text.Encoding]::UTF8.GetBytes("#$Env $State $Ctx`n$Code")
    $c.Write($req, 0, $req.Length)
    $c.Flush()
    $ms = New-Object IO.MemoryStream
    $buf = New-Object byte[] 65536
    do { $n = $c.Read($buf, 0, $buf.Length); $ms.Write($buf, 0, $n) } while (-not $c.IsMessageComplete -and $n -gt 0)
    $reply = [Text.Encoding]::UTF8.GetString($ms.ToArray())
    $status, $body = $reply -split "`n", 2
    $body
    if ($status -ne 'OK') { exit 1 }
} finally { $c.Dispose() }
