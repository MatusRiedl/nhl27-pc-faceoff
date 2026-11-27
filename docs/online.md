# Online play (CrossCrease server)

NHL 27's online modes talk to EA's servers, which an emulated PS5 can't reach. The
[CrossCrease](https://github.com/CrossCrease) server (`nhl27.crosscrease.com`) stands in for them: sign-in,
Online Versus matchmaking, results and records. The match itself runs directly between the two players' PCs.

Status: **test.** Sign-in, matchmaking and full Online Versus matches with saved results work between two PCs.
Other online modes are not supported yet.

## Play

1. Start Faceoff once so a build is installed, and play offline once to check the game runs.
2. Double-click `Play-NHL27-Online.cmd` in the Faceoff folder.
3. First start only: choose your online ID (3-16 letters, digits, `-` or `_`). Your first sign-in claims that
   name on the server.
4. Main menu → **Online Versus** → **Play Online Versus**. Pick team, jersey and playbook and confirm your
   controls; the search starts by itself. Two players searching in the same mode at the same time are paired.

Online uses the same build, settings (`cfgGlobal.json`), saves and shader cache as offline play.

## Requirements

- NHL 27 (PS5, PPSA34063) update **01.003.001**. The script stops with a message for another version.
- Both players on the same Faceoff build, so both games compute the match the same way.
- Windows Firewall: allow `kyty_emulator.exe` when Windows asks (private and public networks).
- Matches run over **UDP 3659** between the two PCs. If you are paired but the match never starts, forward UDP
  3659 on your router to your PC (the game also tries UPnP).
- VPN software (Tailscale, ZeroTier) can stay on: the emulator uses the network card Windows routes Internet
  traffic through.

## Your files

| File (Faceoff folder) | What it is |
|---|---|
| `crosscrease-identity.json` | Your online ID and the secret that proves it is yours. **Keep it, never share it.** Copy it to a new PC to keep your name; without it the name can't be used again. |
| `crosscrease-online.json` | The configuration of the last online start (includes the secret). Rewritten at every start. |
| `crosscrease-online.log` | The online trace. Send it with `play_log.txt` when something fails. |

## How it works

- `tools/nhl27/play-nhl27-online.ps1` starts the same build as `play-nhl27.ps1`, with `--network-connected`,
  `--user-name <online id>`, `--game-patch crosscrease/nhl27-crosscrease-trust.json` and
  `CROSSCREASE_ONLINE_CONFIG` pointing at the per-run configuration.
- `crosscrease/crosscrease-online.json` maps EA's host names to the server and blocks every other name. The
  script resolves `nhl27.crosscrease.com` to its IPv4 address at each start, so a server move needs no update.
- `crosscrease/nhl27-crosscrease-trust.json` replaces the public key of the certificate authority the game trusts
  for its EA connections with the CrossCrease server's, in memory, for this launch only. Game files are never
  changed. It holds no private key.
- The emulator's CrossCrease online code (`src/libs/crosscreaseOnline.*` and the marked hooks in `network.cpp` and
  `libNet.cpp`) does nothing unless `CROSSCREASE_ONLINE_CONFIG` is set: offline play is unchanged.

To point the start script at another CrossCrease server, replace the server name in `crosscrease-online.json` and
use the trust patch that server generates.
