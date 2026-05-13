# Roadmap

Ursprüngliche Schätzung war 2-4 Monate Solo-Arbeit bis zur ersten
funktionierenden Cisco-Verbindung. Tatsächlich gebraucht: **2 Tage**
(Initial Commit 2026-05-02, M7 abgeschlossen 2026-05-03 abend).
Erklärung des Tempos: stark unterstütztes Coding-Setup, klare API
(strongSwan's `kernel_libipsec_*` als 1:1-Blueprint), und das
WinDivert-Risiko stellte sich als kleiner heraus als befürchtet.

Was tatsächlich passierte unterscheidet sich an mehreren Stellen vom
ursprünglichen Plan — die nummerierten Milestones bleiben aber als
Anker. Details zu den Stolpersteinen siehe
[`docs/history.md`](docs/history.md).

---

## Abgeschlossen

### M0 — Toolchain & Skeleton ✅

- MSYS2-MINGW64 mit gcc, cmake, qt6-base, autotools (Setup-Script)
- Repo-Skeleton, CMake-Root, `.gitignore`
- Hello-Qt-Window baut → Qt-Stack validiert
- Hello-MinGW-C-Programm baut → C-Toolchain validiert

### M1 — strongSwan vendored + charon-svc baut ✅

- `third_party/strongswan/` als shallow-Clone (gepinnt auf 6.0.6)
- `scripts/build-charon.sh` mit korrekten configure-Args für MinGW
  (`--enable-monolithic`, `--enable-svc`, `--enable-socket-win`,
  `--enable-kernel-iph`, etc.)
- `charon-svc.exe` startet als Console-App
- Erste swanctl-Smoke-Tests

**Stolpersteine:** strongSwans Windows-Port ist nur eingeschränkt
gepflegt; mehrere autotools-Patches mussten in `build-charon.sh`
landen (configure.ac-Edits, libcharon/Makefile.am-SUBDIRS-Erweiterung,
plugin_constructors.c-Cache-Invalidierung).

### M2 — Wintun-Spike ✅

- Standalone C-Programm in `src/wintun-spike/`
- Lädt `wintun.dll`, erzeugt Adapter, setzt IP, liest/schreibt Pakete
- Validiert Treiber-Auto-Install ohne Reboot

**Risiko gateway eliminiert:** Wintun installiert sauber unter
Windows 11. Größtes Architektur-Risiko vom Tisch.

### M3 — kernel-wintun Plugin ✅

- Plugin-Skeleton in `src/plugin/kernel-wintun/`
- Out-of-tree-Mounting in den strongSwan-Source-Tree via
  `build-charon.sh`
- `wintun_loader.{h,c}` — Runtime-DLL-Loader mit
  CALLBACK-Macro-Push/Pop-Workaround (strongSwans `compat/windows.h`
  undef'd CALLBACK, Wintun braucht `__stdcall`)
- `kernel_wintun_router.c` — Win32-Port von
  `kernel_libipsec_router.c` (`WaitForMultipleObjects` statt `poll`,
  `stop_event` für Clean-Shutdown statt `pthread_cancel` das in
  WaitForMultipleObjects nicht feuert)
- `kernel_wintun_ipsec.c` — kernel_ipsec_t via libipsec

**Architektur-Entscheidung:** kernel-wintun provided nur
`kernel-ipsec`, kernel-iph behält `kernel-net` (mit Patch). Siehe
ARCHITECTURE.md §6.

### M4 — ESP/SA-Pfad ✅

Ursprünglich war hier die WFP-vs-Userspace-Entscheidung geplant.
Tatsächlich war diese Frage in M3 schon de facto getroffen — wenn
kernel-wintun `kernel-ipsec` provided, ist WFP raus. Der "richtige"
M4-Inhalt war: Wintun-Adapter im Plugin verkabeln und libipsec live
schalten.

- `kernel_wintun_plugin.c` schiebt `WintunCreateAdapter` /
  `WintunStartSession` in `PLUGIN_DEFINE`, exposed
  `kernel-wintun-session` über `lib->set`
- Outbound-ESP-Pipe live: ping → wintun-RX →
  `ipsec->processor->queue_outbound` → libipsec encrypt →
  `charon->sender->send_no_marker` → UDP/4500 raus

### M5 — Cisco-Interop ✅

- IKE_SA + CHILD_SA standen am 2026-05-03 mit DRHC's Cisco IOS-XE
- Cert-Auth (RSA 2048), AES-256, SHA-256, MODP-2048, ESP/AES-CBC-256
- NAT-T (UDP/4500) Auto-Detection funktioniert

**Stolpersteine** (Details in `docs/history.md`):
- Cisco hatte zwei IKEv2-Profile mit `match identity remote any` —
  per `match certificate` differenziert
- Stale Server-Cert lokal — frischer DN-Match nötig
- Client-Cert ist `CA:FALSE` → muss in `swanctl/x509/` (nicht
  `x509ca/`)
- `local.id = %fromcert` (nicht hartcodieren — strict rdn_matching
  ist nicht permissiv genug)

### M6 — VIP/Route auf Wintun ✅

- Patch `patches/0001-kernel-iph-vip-install-on-wintun.patch` —
  `add_ip` + `manage_route` in kernel-iph erkennen den
  Wintun-Adapter über LUID + FriendlyName-Fallback
- Outbound-ESP fließt von charon zu Cisco

**Surprise-Bug** (siehe history): kernel-iph's `manage_route()`
matchte Adapter ausschließlich per GUID-AdapterName, kernel-wintun
liefert aber den FriendlyName "dhc-vpn". Silent NOT_FOUND — nichts
im Log, nur dass Routen nie installiert wurden. Fix:
`ConvertInterfaceAliasToLuid` als zweiter Lookup-Pfad.

### M7 — Inbound-ESP-Capture (Original-M7 war "Installer") ✅

Der M7 aus der Original-Roadmap war Installer/Polish; tatsächlich
war zu diesem Zeitpunkt der Tunnel in der Empfangsrichtung tot —
**Cisco's ESP-Antworten kamen am Host an, charon sah sie nie**.
M7 wurde zu "Inbound-ESP-Pfad lösen" umgewidmet.

- WinDivert 2.2.2 vendored unter `third_party/windivert/`
- `src/windivert/windivert_loader.{h,c}` — Runtime-DLL-Loader
- M7 Step 1: Standalone-Spike (`src/windivert-spike/`) bewies dass
  WFP-NETWORK-Layer ESP-in-UDP-Pakete sieht die `tcpip.sys` an die
  Userland nicht ausliefert
- M7 Step 2: `kernel_wintun_capture.c` — WinDivert-Worker im Plugin
  feedet `ipsec->processor->queue_inbound`

**Verifizierung 2026-05-03 abend:**

```
ping 192.168.99.1 -n 8  →  8/8 replies, 16-22 ms RTT
charon SA  in : 698 B / 10 packets
charon SA  out: 798 B / 11 packets
```

**Hypothesen-Korrektur** (siehe history): Die Halb-Tags-Hypothese
war "IPsec.sys frisst die Pakete" wegen "Unzulässige SPI: 858" im
netsh-Counter. Tatsächlich bewegte sich der Counter während des
Tests **nicht** — die ursprünglichen 858 waren accumulated noise.
Der echte Mechanismus ist ein UDP/4500-Demux **in tcpip.sys** der
Pakete mit non-zero ersten 4 Bytes (= ESP SPI) zur kernel-IPsec-
Engine umlenkt anstatt an gebundene Userland-Sockets.

---

## In Arbeit / Geplant

### M8 — Qt6-GUI mit VICI-Anbindung ⏳

Skeleton-MainWindow steht in `src/gui/main_window.{h,cpp}`. Geplanter
Scope:

- VICI-Client als Qt-Klasse, wrappt `libvici-0.dll` (vorhanden in
  `build/charon-install/bin/`). Async via Worker-Thread, Signals
  zurück in den UI-Thread.
- MainWindow:
  - Connection-Dropdown (gefüllt aus `swanctl --list-conns`)
  - Connect/Disconnect-Button → VICI initiate/terminate
  - Status-Label (disconnected/connecting/connected/error)
  - Live-Traffic-Stats (SA in/out bytes, gepollt ~2s)
  - Log-View (charon stderr stream)
- Erste Iteration **ohne**: Profil-Editor, Cert-Picker (CertStore),
  Service-Installation, Auto-Connect. Diese kommen wenn der
  Connect/Disconnect-Pfad steht.

### M9 — Installer & Polish

- M9a (✅) — ZIP installer + `scripts/install.ps1` / `uninstall.ps1`.
  Tester-grade flow.
- M9b (✅) — service-ACL grant für Authenticated Users, GUI
  auto-startet charon-svc, Wintun-Firewall-Regel.
- M9c (✅) — WiX-MSI-Installer (`installer/Product.wxs`,
  `scripts/build-msi.ps1`):
  - charon-svc als Windows-Dienst (LocalSystem, Manual-Start)
  - Service-ACL-Grant via deferred CustomAction (sc.exe sdset)
  - Service-Env-Vars (STRONGSWAN_CONF, SWANCTL_DIR) via
    HKLM\SYSTEM\CurrentControlSet\Services\charon-svc\Environment
  - UDP/500 + UDP/4500 Firewall-Regeln über Firewall-Extension
  - Programm-Verzeichnis `%ProgramFiles%\dhc-vpn\`, User-Daten
    `%ProgramData%\dhc-vpn\swanctl\` (idempotent angelegt, beim
    Uninstall nur entfernt wenn leer)
  - Start-Menü-Verknüpfung
  - MajorUpgrade für Drop-in-Updates ohne Vor-Uninstall
  - Wintun- und WinDivert-Treiber bleiben User-Space-installed
    bei erstem Daemon-Start (kein Treiber-Installer nötig)
- Code-Signing (⚠ aktuell unsigned) — SignPath-OSS-Bewerbung wurde
  am 2026-05-13 wegen fehlender Public-Visibility-Signale abgelehnt.
  `release.yml` hängt aktuell unsigned ZIP + MSI plus SHA256-Sidecars
  an einen GitHub-Release-Draft; SmartScreen-Warnung ist erwartet und
  in `README.md` / `docs/USER_GUIDE.md` für End-User dokumentiert.
  Mittelfristig: Migration auf Azure Trusted Signing oder erneute
  SignPath-Foundation-Bewerbung, sobald das Projekt mehr externe
  Sichtbarkeit hat.

### Offen

- Ereignisprotokoll-Integration (Windows Event Log)
- End-User-Doku: Profil-Import, Zertifikats-Setup, Troubleshooting
- Konfigurierbarer ProgramData-Pfad im MSI (aktuell hardcoded
  `C:\ProgramData\dhc-vpn\` im strongswan.conf-Logpfad)

### Out-of-Scope (vorerst)

- IKEv1 (Cisco hat IKEv2 — falls IKEv1 nötig: separater Issue)
- IPv6-only-Tunnel (IPv4-over-IPv6 sollte funktionieren, aber nicht
  getestet)
- Two-Factor / TOTP-Integration
- macOS/Linux-GUI-Builds (Qt-Code könnte später wiederverwendet
  werden, aber das Plugin und der Capture-Pfad sind streng
  Windows-spezifisch)
- Profile-Sharing-Features (URL-basierter Profile-Import etc.)

---

## Zeitleiste (real)

Quelle: `git log --reverse --pretty=format:'%ad  %h  %s' --date=short`.

| Datum | Commit | Was passierte |
|---|---|---|
| 2026-05-02 | `e835a54` | Initial commit |
| 2026-05-02 | `16ae297` | Initial scaffolding (M0-M2) |
| 2026-05-02 | `663b21e` | M3.2: kernel-wintun plugin skeleton + plugin-API doc |
| 2026-05-02 | `06ded63` | M3.3+M3.7: kernel-wintun in strongSwan-Build, charon-svc startet |
| 2026-05-02 | `c7ddb13` | M3.4: kernel-iph patch für VIP-Install auf Wintun |
| 2026-05-02 | `e8a0126` | M3.5: kernel_wintun_ipsec.c — Port von kernel_libipsec_ipsec.c |
| 2026-05-03 | `28de42d` | M3.6: kernel_wintun_router.c — Win32-Port (WaitForMultipleObjects) |
| 2026-05-03 | `dcd209b` | M4: end-to-end pipeline live — Wintun adapter wired up |
| 2026-05-03 | `f81b655` | M5: Cisco IKE_SA established |
| 2026-05-03 | `978adbf` | M6 step 1: kernel-iph FriendlyName-Match |
| 2026-05-03 | `6902351` | kernel-wintun clean shutdown via stop_event |
| 2026-05-03 | `67bd4bd` | M6 step 3 + diagnostics |
| 2026-05-03 | `a0aa4a8` | M7: WinDivert capture — Tunnel komplett ✅ |

**Insgesamt: 2 Tage** (2026-05-02 → 2026-05-03) vom leeren Repo bis
zum funktionierenden End-to-End-Tunnel mit Cisco. M5 (Cisco-Konfig)
und M7 (Inbound-Drop-Diagnose mit Hypothesen-Korrektur) waren
trotzdem die zwei intensivsten Phasen — beide allerdings am
2026-05-03 selbst gelöst.
