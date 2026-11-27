# Starts NHL 27 online on the CrossCrease server (nhl27.crosscrease.com, https://github.com/CrossCrease).
# Same settings as play-nhl27.ps1 (cfgGlobal.json), plus the CrossCrease online configuration:
#   crosscrease\crosscrease-online.json        server address and host map (no secrets)
#   crosscrease\nhl27-crosscrease-trust.json   lets NHL 27 trust the server (applied in memory, game files untouched)
# The first start asks for your online ID. The secret that keeps the name yours is stored in
# crosscrease-identity.json in the Faceoff folder: keep it (copy it to a new PC to keep your name) and never share it.
# Logs: play_log.txt and crosscrease-online.log in the Faceoff folder.
param(
	[string]$GamePath = "",
	[string]$OnlineId = "",
	# Only for two players on one PC: a different loopback address each (127.0.0.1, 127.0.0.2) and folder each.
	[string]$LocalAddress = "",
	[string]$PlayerDirectory = "",
	# Two emulators on one PC: don't pin both to the same CPU cores.
	[switch]$ConcurrentClients
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot

# The Faceoff folder (saves, cache, logs) is the working folder, as for offline play. The script sits either in
# it or in builds\<build>\.
$root = $here
if ((Split-Path -Leaf (Split-Path -Parent $here)) -eq "builds") { $root = Split-Path -Parent (Split-Path -Parent $here) }

# The emulator: the one next to this script, else the build Faceoff last picked, else the highest numbered build.
$buildDir = $null
if (Test-Path -LiteralPath (Join-Path $here "kyty_emulator.exe")) {
	$buildDir = $here
} else {
	$builds = Join-Path $root "builds"
	$picked = $null
	$state  = Join-Path $root "faceoff.json"
	if (Test-Path -LiteralPath $state) {
		try { $picked = (Get-Content -LiteralPath $state -Raw | ConvertFrom-Json).build } catch { }
	}
	if ($picked -and (Test-Path -LiteralPath (Join-Path $builds "$picked\kyty_emulator.exe"))) {
		$buildDir = Join-Path $builds $picked
	} elseif (Test-Path -LiteralPath $builds) {
		$buildDir = (Get-ChildItem -LiteralPath $builds -Directory |
			Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "kyty_emulator.exe") } |
			Sort-Object { [int]("0" + ($_.Name -replace '[^0-9]', '')) } | Select-Object -Last 1).FullName
	}
}
if (-not $buildDir) { Write-Host "No kyty_emulator.exe found. Start Faceoff once so it installs a build."; exit 1 }
$exe = Join-Path $buildDir "kyty_emulator.exe"

$onlineDir = Join-Path $here "crosscrease"
if (-not (Test-Path -LiteralPath $onlineDir)) { $onlineDir = Join-Path $buildDir "crosscrease" }
$template = Join-Path $onlineDir "crosscrease-online.json"
$trust    = Join-Path $onlineDir "nhl27-crosscrease-trust.json"
foreach ($file in @($template, $trust)) {
	if (-not (Test-Path -LiteralPath $file)) { Write-Host "Missing $file. Reinstall Faceoff."; exit 1 }
}

function Test-GameFolder([string]$path) { return $path -and (Test-Path -LiteralPath (Join-Path $path "eboot.bin")) }
$saved = Join-Path $here "game_path.txt"
foreach ($candidate in @($saved, (Join-Path $root "game_path.txt"))) {
	if (-not $GamePath -and (Test-Path -LiteralPath $candidate)) { $GamePath = (Get-Content -LiteralPath $candidate -TotalCount 1).Trim() }
}
if ($GamePath -and (Split-Path -Leaf $GamePath) -eq "eboot.bin") { $GamePath = Split-Path -Parent $GamePath }
if (-not (Test-GameFolder $GamePath)) {
	Add-Type -AssemblyName System.Windows.Forms
	$dialog = New-Object System.Windows.Forms.FolderBrowserDialog
	$dialog.Description = "Select your NHL 27 game folder (the one that holds eboot.bin)"
	if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { exit 1 }
	$GamePath = $dialog.SelectedPath
}
if (-not (Test-GameFolder $GamePath)) { Write-Host "No eboot.bin in $GamePath. Pick the folder that holds eboot.bin."; exit 1 }
Set-Content -LiteralPath $saved -Value $GamePath -Encoding UTF8

# The server's trust patch is made for one game version; both players need the same one.
$paramFile = Join-Path $GamePath "sce_sys\param.json"
if (Test-Path -LiteralPath $paramFile) {
	$param = Get-Content -LiteralPath $paramFile -Raw | ConvertFrom-Json
	$patchVersion = (Get-Content -LiteralPath $trust -Raw | ConvertFrom-Json).version
	if ($param.titleId -and $param.titleId -ne "PPSA34063") { Write-Host "This is not NHL 27 (PPSA34063): $($param.titleId)."; exit 1 }
	if ($param.contentVersion -and $patchVersion -and -not ([string]$param.contentVersion).StartsWith($patchVersion)) {
		Write-Host "Online play needs NHL 27 update $patchVersion; your game is $($param.contentVersion)."
		exit 1
	}
}

if (-not $PlayerDirectory) { $PlayerDirectory = $root }
New-Item -ItemType Directory -Force -Path $PlayerDirectory | Out-Null
$player = (Resolve-Path -LiteralPath $PlayerDirectory).Path

# Online identity: the name and a random secret, created on the first start. The server binds the name to the
# secret at the first sign-in.
$identityPath = Join-Path $player "crosscrease-identity.json"
$identity = $null
if (Test-Path -LiteralPath $identityPath) { $identity = Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json }
if ($identity -and $OnlineId -and $OnlineId -ne $identity.online_id) {
	Write-Host "This folder already belongs to $($identity.online_id). Use another -PlayerDirectory for a second online ID."
	exit 1
}
if (-not $identity) {
	if (-not $OnlineId) {
		Add-Type -AssemblyName Microsoft.VisualBasic
		while ($true) {
			$OnlineId = [Microsoft.VisualBasic.Interaction]::InputBox(
				"Choose your online ID (3-16 letters, digits, - or _).`nThe first sign-in claims this name on the CrossCrease server.",
				"NHL 27 online", "")
			if (-not $OnlineId) { exit 1 }
			if ($OnlineId -match '^[A-Za-z0-9_-]{3,16}$') { break }
		}
	}
	if ($OnlineId -notmatch '^[A-Za-z0-9_-]{3,16}$') { Write-Host "Invalid online ID '$OnlineId'."; exit 1 }
	$bytes = New-Object byte[] 24
	[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
	$secret = [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
	$identity = [pscustomobject]@{ online_id = $OnlineId; secret = $secret }
	$identity | ConvertTo-Json | Set-Content -LiteralPath $identityPath -Encoding UTF8
	Write-Host "Created online identity $OnlineId. Keep crosscrease-identity.json: it proves the name is yours."
}

# Per-run configuration: names resolved to IPv4 (the emulator's host map takes addresses), the identity added.
function Resolve-Ipv4([string]$name) {
	$ip = $null
	if ([System.Net.IPAddress]::TryParse($name, [ref]$ip)) { return $name }
	$found = [System.Net.Dns]::GetHostAddresses($name) | Where-Object { $_.AddressFamily -eq "InterNetwork" } | Select-Object -First 1
	if (-not $found) { throw "No IPv4 address for $name" }
	return $found.IPAddressToString
}
$config = Get-Content -LiteralPath $template -Raw | ConvertFrom-Json
try {
	foreach ($entry in @($config.hosts.PSObject.Properties | Where-Object { $_.Name -ne "_comment" })) {
		$entry.Value = Resolve-Ipv4 ([string]$entry.Value)
	}
	if ($config.http_gateway) {
		$gatewayHost, $gatewayPort = ([string]$config.http_gateway).Split(":")
		$config.http_gateway = "$(Resolve-Ipv4 $gatewayHost):$gatewayPort"
	}
} catch {
	Write-Host "Cannot reach $($config.server): $($_.Exception.Message). Check your Internet connection."
	exit 1
}
# Account id: 2000000000 + the first four bytes of SHA-256(lower-case online ID), as the server computes it.
$hash = [System.Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::ASCII.GetBytes($identity.online_id.ToLowerInvariant()))
$accountId = [uint64]2000000000 + ([uint64]$hash[0] -shl 24) + ([uint64]$hash[1] -shl 16) + ([uint64]$hash[2] -shl 8) + [uint64]$hash[3]
$config.trace_file = Join-Path $player "crosscrease-online.log"
$config | Add-Member -NotePropertyName online_id -NotePropertyValue $identity.online_id -Force
$config | Add-Member -NotePropertyName account_id -NotePropertyValue $accountId -Force
$config | Add-Member -NotePropertyName auth_code -NotePropertyValue "$($identity.online_id):$($identity.secret)" -Force
if ($LocalAddress) { $config | Add-Member -NotePropertyName local_address -NotePropertyValue $LocalAddress -Force }
# Holds the sign-in secret: written next to the identity, rewritten at every start.
$configPath = Join-Path $player "crosscrease-online.json"
$config | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $configPath -Encoding UTF8

# Faceoff's settings for the picked build, read the same way as play-nhl27.ps1.
$screens = @{ R1280X720 = @(1280, 720); R1920X1080 = @(1920, 1080); R2560X1440 = @(2560, 1440); R3840X2160 = @(3840, 2160) }
$arguments = @("--game", "`"$GamePath`"", "--network-connected", "--user-name", $identity.online_id,
	"--game-patch", "`"$trust`"")
foreach ($name in "cfgGlobal.json", "cfgPPSA34063.json") {
	$file = Join-Path $buildDir $name
	if (-not (Test-Path -LiteralPath $file)) { continue }
	$emu = (Get-Content -LiteralPath $file -Raw | ConvertFrom-Json).emu
	foreach ($setting in $emu.env.PSObject.Properties) {
		[Environment]::SetEnvironmentVariable($setting.Name, [string]$setting.Value, "Process")
	}
	if ($screens.ContainsKey([string]$emu.screen_resolution)) {
		$size = $screens[[string]$emu.screen_resolution]
		$arguments += @("--screen-width", $size[0], "--screen-height", $size[1])
	}
	if ($emu.fullscreen_enabled -and -not $ConcurrentClients) { $arguments += "--fullscreen" }
	if ($emu.extra_args) { $arguments += $emu.extra_args }
}
if ($ConcurrentClients) {
	$env:KYTY_GPU_THREAD_CPUS            = "off"
	$env:KYTY_GPU_THREAD_PRIORITY        = "0"
	$env:KYTY_GUEST_OFF_STAGE_CORES      = "0"
	$env:KYTY_GUEST_OFF_COPY_CORE        = "0"
	$env:KYTY_GUEST_OFF_CP_CORES         = "0"
	$env:KYTY_WINDOW_MAXIMIZED           = "0"
	$env:SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS = "0"
}
$env:CROSSCREASE_ONLINE_CONFIG = $configPath

$log = Join-Path $player "play_log.txt"
if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log -Destination (Join-Path $player "play_log_previous.txt") -Force }
Write-Host "Starting NHL 27 online as $($identity.online_id) on $($config.server) ($exe)"
Write-Host "Keep this window open while you play. Logs: play_log.txt and crosscrease-online.log in $player"
Write-Host "Don't send crosscrease-identity.json or crosscrease-online.json to anyone: they hold your sign-in secret."
$process = Start-Process -FilePath $exe -ArgumentList ($arguments -join " ") -WorkingDirectory $player `
	-RedirectStandardOutput $log -RedirectStandardError (Join-Path $player "play_log_errors.txt") `
	-NoNewWindow -PassThru
$process.WaitForExit()
if ($process.ExitCode -ne 0) {
	Write-Host "The game stopped with code $($process.ExitCode). Keep play_log.txt and crosscrease-online.log for the bug report."
	exit 1
}
