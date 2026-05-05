# Projekt-Reise: was uns überrascht hat

Chronologische Notizen zu den Stolpersteinen und Hypothesen-Korrekturen
auf dem Weg vom leeren Repo zum funktionierenden Cisco-Tunnel. Diese
Notes sind absichtlich nicht in der Roadmap — Roadmap dokumentiert was
wir vorhatten, dieses Dokument dokumentiert was uns *überrascht* hat
und wie wir es gelöst haben.

Wenn ihr das Plugin später ausbaut oder gegen ein anderes Headend
stellt, hilft euch hier wahrscheinlich mehr als die ARCHITECTURE-Doku.

---

## Vorgeschichte: warum nicht Windows-RAS?

Der Auslöser des Projekts: 12 h erfolgloses Debugging Windows-RAS-IKEv2
gegen den Cisco IOS-XE-Router des Kunden mit Cert-Auth. RAS gibt keine
nützlichen Fehler aus — der Tunnel kommt nicht zustande, Eventlog ist
nutzlos, kein Wireshark möglich für die Crypto-Phase.

Nach einem Tag Recherche stellte sich heraus: Windows-RAS hat eine
sehr eingeschränkte Cipher-Auswahl, eine harte Cert-EKU-/SAN-Prüfung,
und ist mit IKEv2-Fragmentation finicky. Cisco IOS war zwar
korrekt konfiguriert (verifiziert mit anderen Clients), aber
Windows-RAS war einfach nicht überzeugbar.

strongSwan war sofort der Plan-B — bekannt-funktionierende
Cisco-Interop, freier Code, aber laut offizieller strongSwan-Doku
[`README_LEGACY.md`] auf Windows nicht als Road-Warrior-Client
nutzbar weil `kernel-iph::add_ip` ein Stub ist.

Vorhandene Workarounds im Web (alte Strongswan-Forks) waren tot
(404 oder unmaintained). Also: selber bauen. Realistische Schätzung
am 2026-05-02 morgens: 2-4 Monate Solo-Arbeit. Tatsächlich:
**2 Tage** (2026-05-02 Initial-Commit → 2026-05-03 abend
Tunnel-up). Erklärung siehe ROADMAP.md.

---

## M3 — die push_macro/pop_macro-Geschichte

Beim ersten Versuch `wintun.h` ins Plugin zu inkludieren: die Wintun-
API-Typedefs nutzen `WINTUN_*_FUNC` mit `CALLBACK`-Calling-Convention
(= `__stdcall`). strongSwans `utils/compat/windows.h` macht aber ein
`#undef CALLBACK` (Konflikt-Auflösung mit einem strongswan-internen
Macro `CALLBACK` für strongSwan-Coroutine-Style). Resultat:
Wintun-Funktionspointer kompilierten als `__cdecl` statt `__stdcall`,
LoadLibrary klappte, GetProcAddress klappte, Aufruf crashte mit
Stack-Korruption.

Lösung in `wintun_loader.h`:

```c
#pragma push_macro("CALLBACK")
#undef CALLBACK
#define CALLBACK __stdcall
#include "wintun.h"
#pragma pop_macro("CALLBACK")
```

…sowie der Constraint dass `wintun_loader.h` **vor** allen
strongSwan-Headern inkludiert werden muss, damit `winsock2.h` /
`windows.h` in der richtigen Reihenfolge gezogen werden bevor irgendein
strongSwan-Header die compat-Sachen anzieht.

Diese Erkenntnis gilt 1:1 auch für `windivert_loader.h` aus M7 — auch
da steht der Include-Order-Hinweis als Kommentar drin.

---

## M3 — der Stale plugin_constructors.c-Bug

strongSwans Build-System generiert `src/libcharon/plugin_constructors.c`
mit einer Liste aller eingebauten Plugins. Das wird durch ein
Python-Script (`maintenance/regen_constructors.py`) gemacht — aber
**nur** wenn das Python-Script frisch ist. Wenn man `configure` neu
laufen lässt nachdem `--enable-kernel-wintun` hinzugefügt wurde,
regenerated das Build-System die Liste **nicht** automatisch, weil das
Script selbst sich nicht geändert hat.

Resultat: Plugin baut, wird in libcharon einkompiliert, aber wird beim
Plugin-Loading nie initialisiert weil sein Constructor nicht in der
generierten Liste steht. Symptom: charon-svc startet sauber, lädt
brav alle anderen Plugins, aber `kernel-wintun` taucht nirgends auf.

Workaround in `scripts/build-charon.sh`:

```bash
rm -f src/libcharon/plugin_constructors.c \
      src/libstrongswan/plugin_constructors.c
```

…wenn wir die autotools-Inputs gerade gepatcht haben. Dann generiert
make die Datei neu.

Das ist ein Tag-killer wenn man's nicht weiß. Mehrfach gegen die Wand
gelaufen.

---

## M5 — die Cisco-Profil-Catch-All-Falle

Cisco IOS-XE Konfiguration für IKEv2 über mehrere Auth-Methoden hatte
zwei `crypto ikev2 profile` Definitionen:

- `IKEV2-PROF` — für EAP-MS-CHAPv2 (für Mobile-Clients), mit
  `match identity remote any`
- `IKEV2-PROF-CERT` — für Cert-Auth (uns), auch mit
  `match identity remote any`

IOS wählt das erste matching Profil — und beide matchten "any".
Resultat: unser Cert-Auth-Versuch landete im EAP-Profil, das natürlich
keine Cert-Authentifizierung erlaubte → IKE_AUTH-Failure ohne klaren
Grund (Cisco-Side: "no acceptable proposal").

**Fix:** PROF-CERT spezifischer machen mit
```
match certificate MAP-DRHC-CLIENT
```
wo `MAP-DRHC-CLIENT` ein `crypto pki certificate map` ist der nach
`issuer-name co cn = drhc-vpn-ca` filtert. Damit wird PROF-CERT
spezifischer als PROF-EAP, und IOS wählt korrekt aus.

Lehre: **`match identity remote any` ist ein Anti-Pattern** wenn man
mehrere Profile mit unterschiedlicher Auth hat. Immer mit `match
certificate` oder `match fvrf` o.ä. spezifizieren.

---

## M5 — `local.id = %fromcert` (nicht hartcoden)

Erste Versuche mit `local.id = "C=DE, O=DRHC, CN=windows-tobias"` als
RFC4514-DN — von Cisco mit `IKEV2-FAILURE: identity mismatch` abgelehnt,
obwohl der DN wirklich exakt so im Cert stand.

Ursache: strongSwans `strict_rdn_matching` interpretiert das nicht
permissiv (Ordering, Whitespace, Encoding-Detail könnten alle schon
ein Mismatch sein). Lösung: `local.id = %fromcert` sagt strongSwan
"nimm einfach die ID aus dem Cert direkt", und die Bytes matchen dann
exakt was Cisco im Cert sieht.

---

## M5 — CA:FALSE-Cert in `swanctl/x509/`, nicht `x509ca/`

strongSwans Cert-Loading unterscheidet zwischen
`/etc/swanctl/x509ca/` (CA-Certs, mit `BasicConstraints CA:TRUE`) und
`/etc/swanctl/x509/` (End-Entity-Certs, `CA:FALSE`).

Unser eigenes Client-Cert war (korrekt) `CA:FALSE`. In der ersten
Konfig hatten wir es nach `x509ca/` gelegt — strongSwan loaded es,
aber refused es als Auth-Cert zu nutzen. Symptom: `--list-certs` zeigt
das Cert, aber `--initiate` schickt es nicht im Cert-Payload. Cisco
sieht keinen Client-Cert.

Fix: `windows-tobias.crt` nach `x509/`, dann referenziert mit
`local.certs = windows-tobias.crt` in der Connection-Section.

---

## M6 — der silent FriendlyName-Mismatch

Plugin-Patch für `kernel-iph::manage_route()` musste den Wintun-Adapter
finden um Routen darauf zu setzen. Der originale Code matchte per
GUID:

```c
if (streq(adapter->AdapterName, params->iface_name)) { ... }
```

`AdapterName` ist auf Windows die GUID-String-Repräsentation
(`{12345678-...}`). `params->iface_name` hingegen kommt aus
`kernel_wintun_router::get_tun_name()` und ist der **FriendlyName**
("dhc-vpn"). Match-Schleife terminiert ohne Match → silent
NOT_FOUND, keine Fehler-Meldung im Log, Route wird nicht installiert,
ping schlägt fehl mit Timeout.

Tag-killer-Bug, weil `[KNL] installed VIP` im Log brav success
meldet (das ist `add_ip`, nicht `add_route`!), aber der nächste
Schritt — die Route — silent failt.

Fix: Win32-Fallback `ConvertInterfaceAliasToLuid` →
`ConvertInterfaceLuidToIndex` als zweiter Lookup-Pfad wenn
GUID-Match scheitert. Patch in
`patches/0001-kernel-iph-vip-install-on-wintun.patch`.

---

## M6 — `pthread_cancel` in `WaitForMultipleObjects` feuert nicht

Erster Wurf des `kernel_wintun_router`-Worker-Threads nutzte
`callback_job_cancel_thread` (das default-Sentinel) als
Cancel-Callback. Resultat beim Plugin-Teardown: charon-svc hängt im
`WaitForMultipleObjects(INFINITE)` und wird nie clean beendet.

Grund: `callback_job_cancel_thread` ruft am Ende `pthread_cancel()` auf
(via winpthreads). pthread_cancel auf Windows funktioniert aber nur
an "Cancellation Points" — und WaitForMultipleObjects ist keiner.
Der Thread blockiert weiter, der Job-Framework-Wait läuft in einen
Timeout.

Fix: eigene `cancel_worker()` Funktion die `SetEvent(stop_event)` macht
und `TRUE` returniert (= "ich beende mich selbst, nur joinen, kein
pthread_cancel"). Der Worker hat `stop_event` als handle[0] in seinem
WFMO-Set, wacht sofort auf, returned `JOB_REQUEUE_NONE`.

Dasselbe Pattern wurde später in `kernel_wintun_capture` für
`WinDivertRecv` reverwendet — auch da ist die Recv-Funktion kein
Cancellation-Point. Statt `stop_event` macht das Capture-Plugin
`WinDivertShutdown(handle, RECV)`, was den blocking Recv mit
ERROR_NO_DATA returnen lässt.

---

## M7 — die "IPsec.sys frisst alles"-Hypothese (war falsch)

`netsh ipsec dynamic show all` zeigte:
```
Aktive Assoziationen   : 0
Unzulässige SPI-Pakete : 858
```

Hypothese: Windows-Kernel-IPsec.sys greift inbound ESP im Stack ab,
prüft seine SA-DB (leer weil charon SAs in Userland hält), markiert
sie als "unzulässige SPI" und droppt sie. Das war auch der
Original-Plan für M7 — Pfade zu finden um IPsec.sys zu umgehen
(zunächst dachten wir an Driver-Disable, dann an WinDivert).

Tatsächlich (verifiziert während M7-Step-1-Spike):
- Spike sah 10 ESP-in-UDP-Pakete inbound (matching der
  negotiated SPI)
- charon's SA `in`-Counter blieb auf 0 — die Pakete kamen nicht bei
  charon's UDP-Socket an
- "Unzulässige SPI"-Counter **bewegte sich um 0** während des Tests

Das letzte ist die wichtige Korrektur. Der 858-Wert vom Vormittag war
accumulated Noise aus früheren Tests (oder anderen Quellen — System-
Boot, andere VPN-Versuche). Der echte Drop-Mechanismus inkrementiert
diesen Counter **nicht**.

**Korrigierte Hypothese** (siehe ARCHITECTURE.md §2.5): Windows hat
einen UDP/4500-Demux **in tcpip.sys** der NAT-T-Pakete nach erstem
4-Byte-Inhalt sortiert:
- first4 == 0 (IKE Non-ESP-Marker) → an gebundenen UDP-Socket
- first4 != 0 (ESP-in-UDP) → an Kernel-IPsec-Engine (leer in unserem
  Setup) → silent black hole

Das passiert auch ohne aktive IKEEXT/PolicyAgent/Firewall — ist
tcpip-stack-immanent.

WinDivert ist also nicht "vor IPsec.sys" — WinDivert *ist* der
Userland-Delivery-Path den Windows für ESP-in-UDP nicht bietet.

Lehre: **Counter-Werte ohne Baseline sind irreführend.** "858
Unzulässige SPI" sieht beweisend aus, ist es aber ohne
Vorher/Nachher-Vergleich nicht.

---

## M7 — `WinDivertHelperParsePacket`-Argument-Liste

12 Parameter, von denen 10 NULL-able sind. Beim ersten Versuch
mehrere falsch gesetzt (Reihenfolge IPv4/IPv6/proto/ICMP/...). Compile
sauber durch (alle Pointer-Typen passen), Runtime-Verhalten still:
parse-fail bei jedem Paket, payload bleibt NULL.

Lösung: Argument-Liste exakt aus dem WinDivert-Header abschreiben,
keine clever-pointers, keine NULL-Mischung erraten:

```c
WinDivertHelperParsePacket_p(pkt, pkt_len,
    &ip4, NULL, NULL, NULL, NULL, NULL, &udp,
    &payload, &payload_len, NULL, NULL);
```

Reihenfolge: `ipv4, ipv6, protocol, icmp, icmpv6, tcp, udp, data,
data_len, next, next_len`.

Tipp: für so eine Funktion einmal die Header-Doku sklavisch durchgehen
und nicht nach Gefühl gehen.

---

## Querschnitt-Lehren

1. **Silent failures sind Tag-killer.** kernel-iph's NOT_FOUND ohne
   Log-Eintrag. plugin_constructors.c stale ohne Build-Warnung.
   `cp -u … || true` das alte DLLs stehenlässt (siehe
   ARCHITECTURE.md). Wenn etwas nicht funktioniert und es gibt kein
   Log darüber — **das** ist die Stelle die zu instrumentieren ist.

2. **Windows-Subsysteme ohne Doku reverse-engineer-en kostet Tage.**
   Der UDP/4500-Demux in tcpip.sys ist nirgendwo offiziell
   dokumentiert. Microsoft-Engineering-Blogs erwähnen ihn am Rande.
   Die einzige Quelle die ich fand war ein WinDivert-Issue-Thread.
   Erkenntnisse aus solchen Reverse-Engineering-Sitzungen unbedingt
   ins Repo dokumentieren — `docs/history.md` ist genau dafür da.

3. **Counter ohne Baseline → nutzlos als Beweis.** "858 Unzulässige
   SPI" sah verdammt überzeugend aus. War es nicht.

4. **strongSwans `kernel_libipsec_*` ist ein verdammt guter Blueprint.**
   Linux-Implementierung hat jedes Pattern das wir gebraucht haben:
   Worker-Thread, ESP-RX-Callback, deliver_plain, send_esp,
   queue_inbound/outbound. Der eigentliche Win32-Port war 80%
   Pattern-Match.

5. **WinDivert ist solide.** Filter-Sprache funktioniert wie
   dokumentiert (`udp.Payload[N] != 0` parsed sauber). Driver-Install
   ohne Reboot funktioniert. SeLoadDriverPrivilege-Check ist klar.
   Keine Überraschungen — die Hauptzeit ging in **was wir damit
   tun** (Hypothese-Korrektur), nicht in **wie wir es benutzen**.

6. **Der Plan ändert sich.** Original-M7 war Installer; tatsächlich-M7
   wurde Inbound-ESP-Capture. Das ist gesund — Roadmap-Treue ist
   nicht Selbstzweck.
