# dhc-vpn — Benutzerhandbuch

IKEv2-VPN-Client für Windows. Diese Anleitung führt durch Installation,
Profilanlage (Zertifikat oder Benutzer/Passwort) und Verbindungsaufbau.

Für Architektur und Build-Hintergrund siehe
[`README.md`](../README.md), [`ARCHITECTURE.md`](../ARCHITECTURE.md)
und [`docs/history.md`](history.md).

---

## Inhalt

1. [Installation](#installation)
2. [Erster Start](#erster-start)
3. [Wo liegen meine Daten?](#wo-liegen-meine-daten)
4. [Profil anlegen](#profil-anlegen)
   - [Zertifikat (Pubkey)](#zertifikat-pubkey)
   - [Benutzer + Passwort (EAP-MSCHAPv2)](#benutzer--passwort-eap-mschapv2)
5. [Verbinden, Trennen, Aktualisieren](#verbinden-trennen-aktualisieren)
6. [Tray-Icon](#tray-icon)
7. [Profile bearbeiten oder löschen](#profile-bearbeiten-oder-löschen)
8. [Logs](#logs)
9. [Fehlerbehebung](#fehlerbehebung)
10. [Deinstallation](#deinstallation)

---

## Installation

Empfohlen ist der MSI-Installer. Den ZIP-basierten Pfad (über
`scripts\install.ps1`) gibt es weiterhin für Spezialfälle, ist aber
nicht mehr Standard.

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
   Get-FileHash .\dhc-vpn-<version>.msi -Algorithm SHA256
   ```
   Der ausgegebene Hash muss zeichengenau mit dem in der `.sha256`-
   Datei übereinstimmen.
2. Im SmartScreen-Dialog auf **Weitere Informationen** → **Trotzdem
   ausführen** klicken.

Mittelfristig ist Authenticode-Signing über Azure Trusted Signing
oder eine erneute SignPath-Foundation-Bewerbung geplant, sobald das
Projekt mehr externe Sichtbarkeit hat.

### MSI

1. `dhc-vpn-<version>.msi` aus dem GitHub-Release herunterladen.
2. Doppelklick (oder `msiexec /i dhc-vpn-<version>.msi`).
3. UAC bestätigen.

Die MSI legt an:

- `C:\Program Files\dhc-vpn\` — Programmdateien (read-only).
- `C:\ProgramData\dhc-vpn\logs\` — `charon.log` des Daemons.
- Windows-Dienst `charon-svc` (Manueller Start, läuft als LocalSystem).
- Firewall-Regeln für UDP/500 und UDP/4500 (eingehend, alle Profile).
- Start-Menü-Eintrag „dhc-vpn".

Profile, Zertifikate und Schlüssel werden **nicht** vom Installer
angelegt — die GUI legt das beim ersten Start unter
`%APPDATA%\dhc-vpn\swanctl\` selbst an.

---

## Erster Start

GUI über das Start-Menü starten („dhc-vpn"). Beim ersten Lauf passiert
automatisch:

1. **Bootstrap**: Verzeichnisbaum unter
   `%APPDATA%\dhc-vpn\swanctl\` (`conf.d\`, `x509\`, `x509ca\`,
   `private\`, `secrets.d\`) plus eine minimale `swanctl.conf` werden
   angelegt.
2. **Migration** (falls aus älteren Versionen Daten unter
   `C:\ProgramData\dhc-vpn\swanctl\` existieren): die werden in den
   neuen User-Pfad kopiert. Eine Zeile im Output-Panel meldet das.
3. **Service-Start**: charon-svc wird per `sc.exe start` automatisch
   gestartet, sobald ein Profil verbunden werden soll. Manuelles
   Starten ist nicht nötig.

Status-Anzeige oben links zeigt eine von vier Zuständen:

- **Disconnected** — getrennt, bereit zum Verbinden.
- **Connecting** — IKE-Aushandlung läuft.
- **Connected** — Tunnel steht.
- **Error** — siehe Output-Panel.

---

## Wo liegen meine Daten?

| Ort | Inhalt | Eigentümer |
|---|---|---|
| `%APPDATA%\dhc-vpn\swanctl\conf.d\` | Profil-Dateien `<name>.conf` | dein User |
| `%APPDATA%\dhc-vpn\swanctl\x509\` | Client- und Peer-Zertifikate (`.crt`/`.pem`) | dein User |
| `%APPDATA%\dhc-vpn\swanctl\x509ca\` | CA-Zertifikate (optional) | dein User |
| `%APPDATA%\dhc-vpn\swanctl\private\` | Private Schlüssel (`.key`/`.pem`) | dein User |
| `%APPDATA%\dhc-vpn\swanctl\secrets.d\` | DPAPI-verschlüsselte EAP-Passwörter (`.dat`) | dein User |
| `C:\ProgramData\dhc-vpn\logs\charon.log` | Daemon-Log | LocalSystem (lesbar für alle) |

Pro Windows-User eigene Profile. Andere User auf demselben Rechner
sehen deine Profile/Schlüssel **nicht** (Default-NTFS-Rechte auf
`%APPDATA%`). EAP-Passwörter sind zusätzlich DPAPI-verschlüsselt mit
deinem User-Schlüssel — selbst wenn jemand eine Kopie der `.dat`-Datei
bekäme, kann er sie ohne deine Anmeldedaten nicht entschlüsseln.

---

## Profil anlegen

**New Profile…** klicken. Der Profil-Dialog öffnet sich.

### Pflichtfelder (beide Modi)

- **Name** — Profilname. Zeichensatz: `A–Z a–z 0–9 _ . -`.
  Wird auch der Dateiname (`<name>.conf`) und der Connection-Bezeichner
  in charon. Beim Bearbeiten gesperrt (Umbenennen geht nur über
  Löschen + neu Anlegen).
- **Remote address** — Hostname oder IP des VPN-Gateways
  (z. B. `vpn.example.com`).

### Optionale Felder (beide Modi)

- **Remote ID** — IKE-Identität des Gateways. Leer lassen, wenn das
  Gateway sich per Cert-Subject identifiziert (üblicher Fall).
  Sonst der Wert, den die Gegenstelle als IDr sendet.
- **Local ID** — eigene IKE-Identität (IDi). Cert-Modus: leer →
  `%fromcert` (Subject aus dem Client-Cert wird verwendet).
  EAP-Modus: leer → der EAP-Username wird übernommen.
- **Remote TS** — Traffic-Selector des Gateways, also welche IPs durch
  den Tunnel sollen. `0.0.0.0/0` = Full-Tunnel (alles), Default.
  Beispiel Split-Tunnel: `10.0.0.0/8`.
- **IKE Proposals** — Algorithmen für IKE-Phase. Default
  `aes256-sha256-modp2048`. Was die Gegenstelle erwartet, ggf. beim
  VPN-Admin erfragen.
- **ESP Proposals** — Algorithmen für ESP-Daten. Default
  `aes256-sha256`.

### Zertifikat (Pubkey)

Auth-Modus **Certificate** auswählen. Vier Felder:

- **Client cert** — dein Client-Zertifikat (`.crt`/`.pem`/`.cer`).
  Datei-Picker; die Datei wird beim Speichern nach
  `%APPDATA%\dhc-vpn\swanctl\x509\` kopiert (du brauchst sie also
  nicht von Hand zu staging).
- **Client key** — der zugehörige private Schlüssel
  (`.key`/`.pem`). Wird nach `private\` kopiert.
- **CA cert** *(optional)* — Root-/Intermediate-CA, gegen die das
  Peer-Zertifikat des Gateways validiert wird. Wird nach `x509ca\`
  kopiert. Leer lassen, wenn die nötigen CAs schon im Windows-Cert-
  Store sind oder du das Gateway-Cert direkt pinnen willst.
- **Remote cert** *(optional)* — Pinnen des konkreten Gateway-Certs
  statt PKI-Validierung. Wird nach `x509\` kopiert. Sinnvoll bei
  selbst-signierten Gateway-Zertifikaten oder wenn du nicht der
  PKI vertraust.

`OK` → das Profil wird als `<name>.conf` in `conf.d\` geschrieben,
Refresh löst automatisch ein `swanctl --load-all` aus, das Profil
erscheint im Dropdown.

#### Schnelltest Cert-Auth

```text
Name             : drhc-cert
Remote address   : vpn.drhc.de
Remote ID        : vpn.drhc.de         (oder leer, wenn aus Cert ableitbar)
Local ID         : %fromcert
Remote TS        : 0.0.0.0/0
Auth mode        : Certificate
Client cert      : C:\…\windows-tobias.crt
Client key       : C:\…\windows-tobias.key
Remote cert      : C:\…\vpn-drhc-de.crt   (Cert-Pinning, kein CA-Setup nötig)
```

### Benutzer + Passwort (EAP-MSCHAPv2)

Auth-Modus **Username + Password** auswählen. Drei Felder:

- **Username** — der EAP-Benutzername. Wird auch als Local ID
  verwendet, wenn das Feld „Local ID" leer ist.
- **Password** — das EAP-Passwort (Klartext im Dialog).
- **Save password** *(Checkbox)* — wenn aktiviert, wird das Passwort
  via Windows-DPAPI (User-Scope) verschlüsselt und unter
  `%APPDATA%\dhc-vpn\swanctl\secrets.d\<name>.dat` abgelegt. Beim
  nächsten Connect wird es automatisch entschlüsselt und an charon
  übergeben.
  Ohne den Haken wirst du bei jedem Connect zur Eingabe gefragt.

Was beim Connect passiert, intern (für Neugierige):

1. DPAPI-Blob (oder Eingabe im Dialog) wird kurz in
   `conf.d\_eap-<name>.secret.conf` materialisiert.
2. `swanctl --load-all` zieht das Secret in den Daemon-Speicher.
3. Die temporäre Datei wird sofort gelöscht.
   Klartext liegt typischerweise <100 ms auf der Platte.

---

## Verbinden, Trennen, Aktualisieren

- **Connect** — startet die IKE-Aushandlung. Wenn `charon-svc` noch
  nicht läuft, wird er per `sc.exe start charon-svc` automatisch
  gestartet (UAC nicht nötig — der Installer hat dafür dem User
  `SERVICE_START` gegeben).
- **Disconnect** — terminiert die SA, der Tunnel geht runter.
- **Refresh** — liest `conf.d\` neu vom Disk und pusht den Stand in
  charon (`swanctl --load-all` + `--list-conns`). Brauchst du, wenn
  du `.conf`-Dateien manuell editiert oder per `cp` ergänzt hast.
  Die GUI selbst macht das nach Edit/New/Delete von Profilen
  automatisch.

Live-Output von swanctl sieht man im Panel unten — IKE-Aushandlung,
ESP-Setup, Routing-Änderungen, Fehlermeldungen, alles dort.

---

## Tray-Icon

Beim Schließen des Hauptfensters minimiert sich die GUI ins
System-Tray, sie läuft also weiter. Rechtsklick auf das Tray-Icon:

- **Show** — Hauptfenster wieder einblenden.
- **Connect** / **Disconnect** — direkter Zugriff auf das aktive
  Profil ohne erst das Fenster zu öffnen.
- **Quit** — beendet die App vollständig.

Beim Doppelklick aufs Icon: zeigt/versteckt das Hauptfenster.

---

## Profile bearbeiten oder löschen

- **Edit…** öffnet den Profil-Dialog mit den aktuellen Werten.
  Der Name ist gesperrt (würde Datei-Renames erfordern, ist im MVP
  nicht abgebildet). Cert-Pfade kannst du gegen neue Dateien
  austauschen — alte Cert-Dateien in `x509\` etc. bleiben dabei
  liegen, kannst du manuell aufräumen.
- **Delete** entfernt `conf.d\<name>.conf` und (falls vorhanden) den
  DPAPI-Secret-Blob `secrets.d\<name>.dat`. Cert/Key-Dateien in
  `x509\`/`private\`/`x509ca\` bleiben liegen, weil andere Profile
  sie noch verwenden könnten. Aufräumen auch hier von Hand.

---

## Logs

Daemon-Log: `C:\ProgramData\dhc-vpn\logs\charon.log` — IKE-Aushandlung,
Plugin-Init, Routing-Änderungen, Fehler. Wird vom `charon-svc`-Dienst
geschrieben (LocalSystem). Bei Ärger immer als Erstes hier reinsehen.

GUI-seitige Live-Ausgabe: das Output-Panel im Hauptfenster zeigt
genau das, was auch der CLI-`swanctl` ausgeben würde — gleicher Pfad,
nur direkt sichtbar.

---

## Fehlerbehebung

### Dropdown bleibt leer trotz vorhandener Profile

1. **Refresh** drücken — löst `--load-all` aus, das `conf.d\` neu liest.
2. Prüfen, dass die `.conf`-Datei tatsächlich in
   `%APPDATA%\dhc-vpn\swanctl\conf.d\` liegt (nicht in
   `%ProgramData%\…` — das ist die alte Location).
3. Output-Panel auf Syntaxfehler in der Conf prüfen — `swanctl`
   meldet jede fehlerhafte Connection einzeln und überspringt sie.

### „charon-svc not running" / Connection refused auf TCP/4502

- Service stoppen/starten als ersten Test:
  ```powershell
  sc stop charon-svc
  sc start charon-svc
  ```
- Wenn er sofort wieder STOPPED meldet: `charon.log` ansehen — meist
  Plugin-Init-Fehler oder fehlende `strongswan.conf`.
- Wenn der Service nicht installiert ist (`sc qc charon-svc` →
  Fehler 1060): MSI nicht oder unvollständig installiert.

### Verbindung baut auf, aber kein Traffic durch den Tunnel

- VIP gesetzt? Im Output-Panel nach `installing virtual IP …`
  suchen. Fehlt das, klemmt der `kernel-iph`-Patch oder Wintun.
- Routing-Tabelle prüfen: `route print` — eine Route über den Wintun-
  Adapter (`dhc-vpn`) sollte da sein.
- Inbound-ESP wird über WinDivert abgegriffen — wenn du eine andere
  Personal-Firewall oder Endpoint-Schutz-Software hast, kann die
  WFP-Hooks blockieren. Probehalber temporär deaktivieren.

### Cert-Auth schlägt fehl mit „no trusted RSA public key found"

- Hast du das Gateway-Cert oder die ausstellende CA hinterlegt?
  Entweder **Remote cert** (Pinning) oder **CA cert** im Profil
  setzen, oder CA in den Windows-Cert-Store importieren.
- Bei selbst-signierten Gateway-Certs ist Pinning der pragmatische
  Weg.

### EAP-Auth schlägt fehl mit „no EAP key found"

- Username im Local-ID-Feld (oder leer für Auto-Übernahme aus
  EAP-Username) konsistent? Manche Cisco-Gateways prüfen IDi gegen
  RADIUS, das Mismatch macht Auth-Reject.
- Save-Password-Blob aus einer alten Version? Profil löschen und neu
  anlegen — dann wird der DPAPI-Blob frisch geschrieben.

---

## Deinstallation

Per Apps & Features (oder `msiexec /x dhc-vpn-<version>.msi`).

Was passiert:
- Programmdateien unter `C:\Program Files\dhc-vpn\` werden entfernt.
- Service `charon-svc` und Firewall-Regeln werden entfernt.
- `C:\ProgramData\dhc-vpn\logs\` bleibt nur erhalten, wenn nicht-leer
  (Daemon-Logs).
- **Deine Profile/Zertifikate/Passwörter unter `%APPDATA%\dhc-vpn\`
  werden NICHT angefasst.** Bei Re-Install greifst du nahtlos auf
  deine Daten zu.

Profile/Zertifikate komplett wegräumen (manuell):

```powershell
Remove-Item "$env:APPDATA\dhc-vpn" -Recurse -Force
```
