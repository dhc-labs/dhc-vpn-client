# dhc-vpn-client

Open-Source IKEv2-VPN-Client für Windows, basierend auf strongSwan. Speziell
gebaut um gegen Cisco-IOS-/ASA-Headends zu funktionieren, mit denen der
Windows-eigene RAS-Client an Cipher- und Cert-Quirks scheitert.

Entwickelt von **Dr. Herbert Consult GmbH** ([drhc.de](https://drhc.de)) als
Teil der `dhc-*` Produktlinie. Repo: [github.com/dhc-labs/dhc-vpn-client](https://github.com/dhc-labs/dhc-vpn-client).

## Status

**Daemon-Side: funktional.** Der Tunnel steht (verifiziert 2026-05-03 gegen
Cisco IOS-XE mit Cert-Auth, NAT-T, ESP/AES-256/SHA-256). Ping durch den
Tunnel: 8/8 Replies, ~16-22 ms RTT. Pro `ping`:

```
charon-svc SA  in : 698 B / 10 packets
charon-svc SA  out: 798 B / 11 packets
```

**GUI: funktional.** Connect/Disconnect, Profile-Editor, System-Tray,
DPAPI-geschütztes EAP-Passwort.

| Komponente              | Stand       | Milestone |
|-------------------------|-------------|-----------|
| MSYS2-Toolchain & CMake | ✅ baut     | M0        |
| charon-svc / swanctl    | ✅ läuft    | M1        |
| Wintun-Treiber-Spike    | ✅ validiert| M2        |
| Plugin `kernel-wintun`  | ✅ läuft    | M3+M4     |
| Cisco IKEv2 Cert-Auth   | ✅ stabil   | M5        |
| VIP/Route auf Wintun    | ✅ über kernel-iph-Patch | M6 |
| Inbound-ESP-Capture     | ✅ über WinDivert | M7  |
| Qt6-GUI (Connect/Disconnect, Profile, Tray) | ✅ läuft | M8 |
| ZIP-Installer + Service + Firewall | ✅ läuft | M9a/b |
| WiX-MSI + GitHub-Actions-Signing-Pipeline | ✅ Scaffolding fertig | M9c |

Details zur Reise siehe [`docs/history.md`](docs/history.md).

End-User-Anleitung (Installation, Profilanlage, Cert-/EAP-Setup,
Troubleshooting): [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md).

## Motivation

Windows-RAS-IKEv2 spricht mit vielen Cisco-Gegenstellen praktisch nicht —
eingeschränkte Cipher-Auswahl, strenge EKU/SAN-Prüfung, Probleme mit
IKEv2-Fragmentation und MODECFG-Attributen.

strongSwans Windows-Port (`charon-svc`) führt die IKE-Aushandlung im
Userspace durch und löst diese Probleme prinzipiell — ist aber laut
offizieller Doku *nicht* als Road-Warrior-Client nutzbar, weil:

1. **`kernel-iph` kann keine virtuellen IPs installieren** (`add_ip` ist ein
   `return NOT_SUPPORTED`-Stub).
2. **Windows NAT-T-Demux schluckt inbound ESP-in-UDP/4500** bevor charon's
   UDP-Socket sie sehen kann.

Dieses Projekt schließt beide Lücken:

1. **Plugin `kernel-wintun`** — virtueller IP + Routing über den Wintun-
   TUN-Treiber (BSD, aus dem WireGuard-Projekt). Patch von `kernel-iph`
   damit es die VIP auf den Wintun-Adapter installiert.
2. **WinDivert-Capture** im selben Plugin — fängt inbound ESP-in-UDP am
   WFP-NETWORK-Layer ab (vor dem Windows-internen UDP/4500-Demux),
   reicht es an libipsec zur Entschlüsselung weiter, liefert das
   Plaintext-IP an den Wintun-Adapter aus.

Outbound nutzt unverändert charon's Standard-IKE-UDP-Socket — der Demux
betrifft nur Empfangsrichtung.

## Architektur (Kurzfassung)

```
   ┌───────────────────────┐         ┌───────────────────────────────┐
   │  Qt6 GUI [M8, ToDo]   │ ──VICI──▶ charon-svc.exe                │
   │  (User-Session)       │         │   - kernel-iph (gepatcht)     │
   └───────────────────────┘         │   - kernel-wintun (eigen)     │
                                     │     - kernel_wintun_router    │
                                     │     - kernel_wintun_capture ──┼─┐
                                     │     - libipsec (ESP crypto)   │ │
                                     │   - socket-win (UDP 500/4500) │ │
                                     └─────────┬─────────────────────┘ │
                                               │                       │
                ESP outbound + IKE             │                       │
                            ┌──────────────────┘                       │
                            ▼                                          │
                   ┌─────────────────┐    inbound ESP-in-UDP/4500      │
                   │  Wi-Fi/LAN NIC  │◀─── (von Cisco) ────────────────┘
                   └────────┬────────┘                       (WinDivert)
                            │
                            ▼
                   ┌─────────────────┐
                   │  Wintun-Adapter │  Plaintext-IP
                   │  'dhc-vpn'      │  ◀── deliver_plain
                   └─────────────────┘
```

Volle Beschreibung: [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Installation

Fertige Builds liegen unter
[GitHub Releases](https://github.com/dhc-labs/dhc-vpn-client/releases):

- `dhc-vpn-<ver>.msi` — empfohlene End-User-Installation. Doppelklick,
  UAC, Start-Menü-Eintrag, registriert `charon-svc` als Windows-Dienst,
  öffnet UDP/500+UDP/4500 in der Firewall.
- `dhc-vpn-<ver>.zip` — Tester-Pfad / scripted Setup. Entpacken und
  `scripts/install.ps1` aus elevatierter PowerShell laufen lassen.

Beides legt das Programm unter `%ProgramFiles%\dhc-vpn\` und Benutzer-
daten (Profile, Zertifikate, Logs) unter `%ProgramData%\dhc-vpn\` ab.
Deinstallation via *Apps & Features* (MSI) oder
`scripts/uninstall.ps1` (ZIP).

### Hinweis zur Code-Signatur

Die Builds sind aktuell **nicht Authenticode-signiert**. Bei Erst-
ausführung zeigt Windows SmartScreen daher eine Warnung
(„Windows hat den Start dieser App verhindert") und/oder Microsoft
Defender meldet den Download.

**Das ist erwartetes Verhalten** für einen jungen Open-Source-Build
ohne etablierte Reputation. So gehst du vor:

1. SHA256-Summe der heruntergeladenen Datei prüfen — die Datei
   `dhc-vpn-<version>.msi.sha256` (bzw. `.zip.sha256`) neben dem
   Release-Asset enthält den erwarteten Hash. PowerShell:
   ```powershell
   Get-FileHash .\dhc-vpn-<ver>.msi -Algorithm SHA256
   ```
2. Im SmartScreen-Dialog auf **Weitere Informationen** → **Trotzdem
   ausführen** klicken.

Mittelfristig planen wir Authenticode-Signing über Azure Trusted
Signing oder eine erneute SignPath-Foundation-Bewerbung, sobald das
Projekt mehr externe Sichtbarkeit hat.

## Build

In einer **MSYS2-MINGW64**-Shell, einmaliges Setup:

```bash
./scripts/setup-msys2.sh    # Toolchain (gcc, qt6, cmake, autotools)
./scripts/fetch-deps.sh     # strongSwan-Source + Wintun-SDK + WinDivert-SDK
```

Build:

```bash
./scripts/build-charon.sh   # baut charon-svc + swanctl + alle Plugins
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build         # baut Qt-GUI-Skeleton + die zwei Spike-Tools
```

`build-charon.sh` deployt automatisch:
- `charon-svc.exe` + Plugins nach `build/charon-install/bin/`
- `swanctl.exe` + Tools nach `build/charon-install/sbin/`
- MinGW-Runtime-DLLs daneben
- `wintun.dll`, `WinDivert.dll`, `WinDivert64.sys` daneben (sonst lädt das
  Plugin nicht)

Lokales Release-Packaging (statt der CI):

```powershell
# in einer regulären PowerShell, vom Repo-Wurzel:
.\scripts\package.ps1       # erzeugt build\dhc-vpn-<ver>.zip + Staging-Tree
.\scripts\build-msi.ps1     # erzeugt build\dhc-vpn-<ver>.msi (unsigned)
```

Die signierten Releases entstehen automatisch beim Push eines `v*`-Tags
(siehe `.github/workflows/release.yml`).

## Laufen lassen

charon-svc.exe braucht **Admin-Rechte**: Wintun rollt seinen Treiber beim
ersten `WintunCreateAdapter` aus, und WinDivert installiert seinen
WFP-Callout-Driver beim ersten `WinDivertOpen`. Im Service-Modus läuft
charon-svc als LocalSystem; für Dev-Tests geht ein elevated Terminal.

```powershell
# elevated PowerShell:
.\scripts\run-charon.ps1     # charon-svc.exe live mit Logs in diesem Terminal

# in einem zweiten elevated Terminal:
.\scripts\test-cisco.ps1     # swanctl --load-all + --initiate + ping-Test
```

Konfiguration: `build/charon-install/etc/swanctl/swanctl.conf` mit den
üblichen `connections.<name>.local`/`remote`/`children` Sektionen. Certs
nach `etc/swanctl/x509/` (Server-Cert / Peer-Cert), `etc/swanctl/private/`
(Client-Private-Key), `etc/swanctl/x509ca/` (CA-Certs falls nötig).

## Test-Skripte

| Skript | Zweck |
|---|---|
| `scripts/run-charon.ps1` | charon-svc.exe in der Konsole starten |
| `scripts/test-cisco.ps1` | Verbindung dialen + ping-Test |
| `scripts/diag-cisco.ps1` | Diagnose: VICI-Probe, list-conns/certs/sas |
| `scripts/m7-spike-test.ps1` | Standalone WinDivert-Capture-Spike (Diagnostic-Tool) |
| `scripts/m7-step2-test.ps1` | End-to-end Plugin-Test (Dial + Ping + Verdict) |

## Was *nicht* im Repo lebt

- **strongSwan-Source** — `scripts/fetch-deps.sh` clont 6.0.6 nach
  `third_party/strongswan/`. Patches gehen über `patches/*.patch` rein.
- **Wintun-SDK** — Binary-ZIP von wintun.net nach `third_party/wintun/`.
- **WinDivert-SDK** — Release-ZIP von github.com/basil00/WinDivert nach
  `third_party/windivert/`.
- **Eigene Certs / Keys / swanctl.conf** — alles unter `build/` und
  `.gitignore`d.

## Lizenz

Eigener Code in diesem Repo: **GPL-2.0-or-later**, siehe [`LICENSE`](LICENSE).
Pflichtwahl, weil das Plugin direkt gegen strongSwan-Internas linkt und
strongSwan unter GPL-2.0-or-later steht.

Vendored / nachgeladene Komponenten behalten ihre Original-Lizenzen:

| Komponente | Lizenz | Bezugsort |
|---|---|---|
| strongSwan 6.0.6 | GPL-2.0-or-later | `third_party/strongswan/` (über `scripts/fetch-deps.sh`) |
| Wintun | GPL-2.0 / Prosperity Public License (vom Hersteller dual) | `third_party/wintun/` |
| WinDivert | LGPL-3.0 / GPL-2.0 (dual) | `third_party/windivert/` |
| Qt6 | LGPL-3.0 (dynamisch gelinkt via `windeployqt`) | MSYS2-Toolchain |
