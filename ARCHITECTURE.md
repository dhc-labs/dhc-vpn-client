# Architektur

Stand der as-built-Implementierung nach M7. Die ursprüngliche
Design-Diskussion (Variante A vs. B, M4-Entscheidung) ist in
[`docs/plugin-api.md`](docs/plugin-api.md) konserviert; dieses Dokument
beschreibt was tatsächlich gebaut wurde und warum.

## Komponenten-Überblick

```
┌─ User-Session ────────────────────────────────────────┐
│                                                       │
│   Qt6 GUI (M8, ToDo)  ────────VICI──────────────┐     │
│                                                  │     │
└──────────────────────────────────────────────────┼─────┘
                                                   │
┌─ LocalSystem-Service ─────────────────────────────▼────┐
│                                                        │
│   charon-svc.exe (strongSwan 6.0.6, MinGW-Build)       │
│   ┌─────────────────────────────────────────────────┐  │
│   │ socket-win        UDP/500 + UDP/4500 IKE        │  │
│   │ kernel-iph        IP-Helper Routes/Adapter      │  │
│   │  └─ Patch         + FriendlyName-Fallback       │  │
│   │                   für add_route auf Wintun      │  │
│   │ kernel-wintun     [eigen]                       │  │
│   │  ├─ plugin        Wintun-Adapter erzeugen       │  │
│   │  ├─ ipsec         kernel_ipsec_t → libipsec     │  │
│   │  ├─ router        wintun ↔ libipsec Pump        │  │
│   │  └─ capture       WinDivert für inbound ESP     │  │
│   │ libipsec          ESP encrypt/decrypt + SAD     │  │
│   └─────────────────────────────────────────────────┘  │
│                                                        │
└────────────────────────────────────────────────────────┘
       │                        │                  ▲
       │ IKE/4500               │ ESP/4500 out     │ ESP/4500 in
       ▼                        ▼                  │ (WinDivert)
   Wi-Fi/LAN NIC ────────────────────────────────  │
       ▲                                           │
       │                                           │
       └─── Cisco IKEv2 Headend (vpn.example.com) ─┘

   Wintun-Adapter "dhc-vpn"  (Layer-3 TUN, BSD-Treiber)
       ▲     ▲                                  │
       │     │ deliver_plain                    │ process_plain
       │     └────── (libipsec → wintun) ◀──── inbound queue
       │                                         │
       │                                         ▼
       │ Plaintext aus User-App                outbound queue
       └──────────────────────────────────────────
```

## 1. `charon-svc` — strongSwan ohne Modifikationen

Der unveränderte strongSwan IKE-Daemon, gebaut für Windows mit MinGW64
(Configure-Args in `scripts/build-charon.sh`). Läuft als Windows-Dienst,
nimmt VICI-Verbindungen über TCP/4502 entgegen, führt die IKEv2-
Aushandlung im Userspace durch.

Aktive Plugins (siehe `--enable-...`-Flags im Build-Script):

| Plugin | Funktion |
|---|---|
| `nonce`, `pem`, `pkcs1`, `x509`, `pubkey` | Crypto-Boilerplate |
| `openssl` | Crypto-Backend |
| `socket-win` | Windows-spezifischer UDP-Socket-Layer |
| `kernel-iph` | IP-Helper-basierter `kernel-net` (Routes, Adapter-Listing) |
| `kernel-wfp` | (geladen, aber displaced — siehe unten) |
| `kernel-wintun` | unsere Implementierung von `kernel-ipsec` |
| `vici` | TCP/4502 Control-Socket |
| `counters`, `swanctl` | Telemetrie / CLI-Anbindung |

Wichtig: strongSwans Plugin-Loader wählt **eine** `kernel-ipsec`-
Implementierung. Wenn `kernel-wintun` provided und resolved wird,
verdrängt es `kernel-wfp` automatisch. WFP wäre für einen
Road-Warrior-Client ohnehin ungeeignet (kein Plaintext-Delivery zu einem
TUN-Adapter aus dem Kernel heraus).

## 2. `kernel-wintun` — das eigene Plugin

Source: `src/plugin/kernel-wintun/`. Build-eingehängt durch
`scripts/build-charon.sh` (out-of-tree → in-tree-Sync, plus
`patches/0001-kernel-iph-vip-install-on-wintun.patch`).

### 2.1 Datei-Layout

```
src/plugin/kernel-wintun/
├── Makefile.am                  Autotools-Sources-Liste
├── kernel_wintun_plugin.{h,c}   Plugin-Lifecycle, PLUGIN_DEFINE
├── kernel_wintun_session.h      wintun_session_handle_t (intern)
├── kernel_wintun_ipsec.{h,c}    kernel_ipsec_t-Implementierung
├── kernel_wintun_router.{h,c}   wintun ↔ libipsec packet-pump
├── kernel_wintun_capture.{h,c}  WinDivert-Worker für inbound ESP
└── (kopiert beim build:)
    wintun_loader.{h,c}, wintun.h
    windivert_loader.{h,c}, windivert.h
```

### 2.2 `kernel_wintun_plugin.c`

`PLUGIN_DEFINE(kernel_wintun)` macht beim Laden:

1. `libipsec_init()` — initialisiert die userspace-IPsec-Bibliothek
2. `wintun_load()` — `LoadLibrary("wintun.dll")` + Symbol-Resolve
3. `WintunCreateAdapter(L"dhc-vpn", L"dhc-vpn")` — Wintun-TUN-Adapter
4. `WintunStartSession()` — 2 MiB Ring-Buffer, Read-Wait-Event
5. `lib->set("kernel-wintun-session")` und `"kernel-wintun-luid"` —
   exposiert Adapter-Handles für `kernel-iph` und `kernel-wintun-router`
6. `lib->settings->set_str("install_virtual_ip_on", "dhc-vpn")` —
   so dass charon's `add_ip()`-Aufruf am gepatchten `kernel-iph` landet

`get_features()` registriert drei Plugin-Features:
- `kernel-ipsec` → `kernel_wintun_ipsec_create`
- `kernel-wintun-router` → `create_router` (Pump-Thread)
- `kernel-wintun-capture` → `create_capture` (WinDivert-Worker)

Letzteres ist **best-effort**: schlägt der WinDivert-Open fehl (Driver
gesperrt, Signature-Check), bleibt das Plugin geladen und IKE
funktioniert — nur inbound ESP fließt nicht. Damit kann man die GUI
auch ohne Admin-Rechte starten und Diagnose machen.

### 2.3 `kernel_wintun_ipsec.c` — `kernel_ipsec_t`

1:1-Port von `kernel_libipsec_ipsec.c`. Delegiert SA-/Policy-Ops direkt
an libipsec's `ipsec->sas` / `ipsec->policies`. Einzige Spezialität:
`install_route()` ruft `wintun_router->get_tun_name(vip)` auf, damit
`kernel-iph::add_route()` die Route korrekt auf den Wintun-Adapter setzt.

### 2.4 `kernel_wintun_router.c` — die Daten-Pump

Win32-Port von `kernel_libipsec_router.c`. Statt POSIX `poll()` nutzt
ein einzelner Worker-Thread `WaitForMultipleObjects()` über:
- `stop_event` — Shutdown-Signal aus `cancel_worker`
- `notify_event` — Adapter-Map hat sich geändert
- `read_event` jedes registrierten Wintun-Sessions

Vier Callbacks (alle in `kernel_wintun_router_create()` registriert):

| Callback | Trigger | Zweck |
|---|---|---|
| `process_plain` (in der Loop) | Wintun signalisiert RX | TUN → libipsec outbound queue |
| `send_esp` | libipsec hat ESP fertig verschlüsselt | → `charon->sender->send_no_marker()` (UDP/4500) |
| `receiver_esp_cb` | charon's UDP-Socket bekommt was, das wie ESP aussieht | → libipsec inbound queue **(Windows: feuert nie für ESP — siehe Capture)** |
| `deliver_plain` | libipsec hat Plaintext-IP fertig entschlüsselt | → `WintunSendPacket()` |

### 2.5 `kernel_wintun_capture.c` — der WinDivert-Worker

**Der entscheidende Teil für Windows.** Source:
`src/plugin/kernel-wintun/kernel_wintun_capture.c`.

Problem: Windows `tcpip.sys` hat einen UDP/4500-Demux der NAT-T-Pakete
nach erstem 4-Byte-Inhalt sortiert:
- `first4 == 0x00000000` (IKE Non-ESP-Marker) → an gebundenen UDP-Socket
- `first4 != 0` (ESP-in-UDP, SPI) → an Kernel-IPsec-Engine (leer in
  unserem Setup, weil charon SAs in libipsec hält) → silent black hole

`receiver_esp_cb` aus dem Router würde also nie feuern; charon's UDP-
Socket sieht ESP-in-UDP gar nicht.

Lösung: WinDivert öffnet einen WFP-NETWORK-Layer-Callout-Handle mit
Filter

```
inbound and ip and udp.DstPort == 4500 and udp.PayloadLength >= 4 and
(udp.Payload[0] != 0 or udp.Payload[1] != 0 or
 udp.Payload[2] != 0 or udp.Payload[3] != 0)
```

der **nur** ESP-in-UDP matcht — IKE-Pakete (first4 = 0) fließen am
Callout vorbei direkt zu charon's Socket.

Worker-Thread:
1. Blockierender `WinDivertRecv()`
2. `WinDivertHelperParsePacket()` zum Extrahieren von IP+UDP-Headern
3. `host_create_from_chunk(AF_INET, ip4->SrcAddr, udp->SrcPort)` für
   Source/Dest mit Port 4500 (libipsec's `esp_packet_create_from_packet`
   nutzt den Port um zu erkennen dass es NAT-T-encapsuliert ist)
4. `packet_create()` mit den Hosts und der UDP-Payload als Daten
5. `ipsec->processor->queue_inbound(esp_packet_create_from_packet(p))`

Ab da ist es derselbe Pfad den `receiver_esp_cb` für IKE feeden würde —
libipsec entschlüsselt, `deliver_plain` schreibt Plaintext-IP in den
Wintun-Adapter, Windows liefert es an die Userland-Anwendung aus.

Lifecycle-Detail: `cancel_worker` ruft `WinDivertShutdown(handle, RECV)`
auf damit der Worker aus seinem Blocking-Recv aufwacht — analog wie
`kernel_wintun_router::cancel_worker` `SetEvent(stop_event)` macht.
Das ist nötig weil winpthreads-Cancellation in `WinDivertRecv()` nicht
feuert (kein Cancellation-Point).

## 3. `kernel-iph` mit Patch

Source: `third_party/strongswan/src/libcharon/plugins/kernel_iph/`,
modifiziert durch `patches/0001-kernel-iph-vip-install-on-wintun.patch`.

Upstream stubt `add_ip` / `del_ip` aus (`return NOT_SUPPORTED`). Der
Patch:

1. `add_ip` — wenn `iface_name == "dhc-vpn"` (oder die FriendlyName
   matcht), nutzt `lib->get("kernel-wintun-luid")` und setzt die VIP per
   `CreateUnicastIpAddressEntry()` auf den Wintun-Adapter.
2. `manage_route()` — bei Adapter-Lookup nicht nur per
   GUID-AdapterName, sondern auch per `ConvertInterfaceAliasToLuid()`
   matchen, damit Routen für FriendlyName "dhc-vpn" auch finden was
   `kernel-wintun` als LUID exposiert.

Dieser zweite Punkt war ein silent-NOT_FOUND-Bug der in M6-step-1
gefunden + behoben wurde — siehe [`docs/history.md`](docs/history.md).

## 4. `socket-win` und IKE

Unverändert. Bindet UDP/500 + UDP/4500 für IKE. Für outbound nutzt
`kernel-wintun` den selben Sender via
`charon->sender->send_no_marker()`.

Der `kernel-wintun-capture`-Filter ist so geschnitten dass IKE-Pakete
**nicht** intercepted werden (first4 == 0 erfüllt das Filter-Predikat
nicht). Also sieht charon's UDP-Socket weiterhin alle IKE-Nachrichten
normal — IKE_AUTH, INFORMATIONAL, DPD, Rekey.

## 5. Vendored Libraries

| Library | Lizenz | Quelle | Vendored unter |
|---|---|---|---|
| strongSwan 6.0.6 | GPLv2 | github.com/strongswan/strongswan | `third_party/strongswan/` |
| Wintun 0.14.1 | BSD | wintun.net | `third_party/wintun/` |
| WinDivert 2.2.2 | LGPLv3 | github.com/basil00/WinDivert | `third_party/windivert/` |

Alle drei werden durch `scripts/fetch-deps.sh` reproduzierbar geladen
(im `.gitignore`d Tree).

## 6. Datenfluss: Verbindungsaufbau und Ping

```
1. swanctl --initiate --child drhc-cert
2. socket-win → IKE_SA_INIT (UDP/500) → Cisco
3. Cisco → IKE_SA_INIT-Antwort
4. IKE_AUTH-Exchange (UDP/4500 nach NAT-T-Detection)
5. Cisco akzeptiert; CHILD_SA wird etabliert
6. charon ruft kernel_wintun_ipsec::add_sa() für inbound + outbound SAs
7. charon ruft kernel_wintun_ipsec::add_policy() — installiert Route auf
   192.168.99.0/24 via Wintun-Adapter (per kernel-iph-Patch + LUID-Lookup)
8. charon ruft kernel-iph::add_ip(192.168.99.139, "dhc-vpn") — die
   gepatchte Variante setzt die VIP auf dem Wintun-Adapter
9. Tunnel up — User pingt 192.168.99.1:
   ┌─ ping.exe sendet ICMP an 192.168.99.1
   │
   ├─ Windows-Routing: Route für 192.168.99.0/24 zeigt auf dhc-vpn
   │  Adapter (ifIndex N) → Paket landet im Wintun-Ring
   │
   ├─ kernel_wintun_router::process_plain liest aus Ring,
   │  ipsec->processor->queue_outbound(ip_packet)
   │
   ├─ libipsec encrypt → ESP-Frame mit SPI X
   │
   ├─ kernel_wintun_router::send_esp(packet, encap=true)
   │  → charon->sender->send_no_marker(packet)
   │  → socket-win → UDP/4500 → Cisco
   │
   ├─ Cisco entschlüsselt, ICMP echo request landet auf 192.168.99.1
   │
   ├─ Cisco antwortet, encrypted, sendet UDP/4500 zurück
   │
   ├─ Bei uns: tcpip.sys würde demuxen, aber:
   │  → kernel_wintun_capture::handle_capture sieht das Paket erst
   │    am WFP-NETWORK-Layer ab
   │  → packet_create + queue_inbound
   │
   ├─ libipsec decrypt → ip_packet mit ICMP echo reply
   │
   ├─ kernel_wintun_router::deliver_plain(packet)
   │  → WintunSendPacket() → Wintun-Adapter
   │
   └─ Windows-Stack liefert ICMP-Reply an ping.exe → "Reply from..."
```

## 7. Was bewusst *nicht* in diesem Plugin lebt

- **Profile-Editor / swanctl.conf-Generierung** — Domäne der GUI (M8).
- **Zertifikats-Picker (Windows-CertStore)** — GUI.
- **Service-Installation / Auto-Start** — Domäne des Installers (M9).
- **Crypto** — `openssl`-Plugin macht das, libipsec hängt sich dort ein.
- **IKE-Policy-Logik** — alles strongSwan-Native; das Plugin sieht nur
  fertige SAs / Policies.

## 8. Was *nicht* im Repo lebt (sondern unter `third_party/`)

Siehe [README.md](README.md) — alle vendored Quellen werden durch
`scripts/fetch-deps.sh` heruntergeladen, im Repo nur die Aufrufe.

## Verzeichnisstruktur (current)

```
dhc-vpn-client/
├── CMakeLists.txt              # Root build (für GUI + Spikes)
├── README.md
├── ARCHITECTURE.md             # this doc
├── ROADMAP.md
├── docs/
│   ├── history.md              # chronologische Reise-Notes
│   └── plugin-api.md           # pre-implementation Design-Reference
├── src/
│   ├── CMakeLists.txt
│   ├── gui/                    # Qt6 Skeleton (M8)
│   ├── wintun/                 # wintun.dll Loader
│   ├── wintun-spike/           # M2 Treiber-Test (Diagnostic)
│   ├── windivert/              # WinDivert.dll Loader
│   ├── windivert-spike/        # M7 Capture-Test (Diagnostic)
│   └── plugin/
│       └── kernel-wintun/      # Das eigentliche Plugin
├── scripts/
│   ├── setup-msys2.sh
│   ├── fetch-deps.sh
│   ├── build-charon.sh         # mountet Plugin + patches in strongSwan-Tree
│   ├── deploy-mingw-deps.sh
│   ├── run-charon.ps1
│   ├── test-cisco.ps1
│   ├── diag-cisco.ps1
│   ├── m7-spike-test.ps1       # WinDivert-only diagnostic
│   └── m7-step2-test.ps1       # full plugin end-to-end
├── patches/
│   └── 0001-kernel-iph-vip-install-on-wintun.patch
└── third_party/                # gitignored, befüllt von fetch-deps.sh
    ├── strongswan/
    ├── wintun/
    └── windivert/
```
