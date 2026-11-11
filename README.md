# Personenzähler – Wemos D1 mini + 2x VL53L1X

## Verdrahtung

| VL53L1X        | Wemos D1 mini |
|----------------|---------------|
| VIN (beide)    | 3V3           |
| GND (beide)    | G             |
| SDA (beide)    | D2            |
| SCL (beide)    | D1            |
| XSHUT Sensor A | D5            |
| XSHUT Sensor B | D6            |

Beide Sensoren hängen am selben I2C-Bus. Beim Start werden sie über XSHUT nacheinander
eingeschaltet und bekommen die Adressen 0x30 (A) bzw. 0x31 (B).

## Montage

```
   AUSSEN                    INNEN
          [A]  ~10-15 cm  [B]
   ---->  Durchgangsrichtung  ---->  = Eintritt
```

- Beide Sensoren nebeneinander in Laufrichtung, z. B. oben im Türrahmen nach unten schauend
  oder seitlich am Rahmen quer über den Durchgang.
- A = aussen, B = innen. Zählt es falsch herum → in der Konfig „Richtung umkehren“ aktivieren.
- Für schnelle Durchgänge lieber etwas mehr Abstand zwischen den Sensoren (15–25 cm), damit
  die beiden Messflecken sich möglichst wenig überlappen.
- Standard ist der Short-Modus (50 Messungen/s, Reichweite ~1.3 m). Nur wenn mehr Reichweite
  nötig ist, „Grosse Reichweite“ aktivieren (30 Messungen/s).
- Die **Auslöseschwelle** (Standard 1000 mm) so wählen, dass sie kleiner ist als der Abstand
  zum Boden / zur gegenüberliegenden Wand, aber eine Person sicher erkennt. Auf der
  Hauptseite werden die Live-Distanzen angezeigt – damit lässt sich das gut einstellen.

## Flashen

Mit [PlatformIO](https://platformio.org/) (VS Code Extension):

```
pio run -t upload
pio device monitor
```

## Erster Start

1. Ohne WLAN-Konfiguration (oder wenn die Verbindung nach 20 s fehlschlägt) öffnet der
   Wemos einen Access Point **`Personenzaehler-xxxxxx`**, Passwort **`zaehler1`**.
2. Verbinden – die Konfigseite öffnet sich meist automatisch (Captive Portal),
   sonst `http://192.168.4.1/config` aufrufen.
3. Anmelden mit Benutzer **`admin`**, Passwort **`zaehler1`**.
4. WLAN und optional MQTT eintragen, **Admin-Passwort ändern**, speichern → Neustart.
5. Danach erreichbar unter `http://personenzaehler.local` bzw. der IP (siehe Router/seriellen Monitor).

Ist ein WLAN konfiguriert, aber nicht erreichbar, versucht das Gerät es im AP-Modus alle
5 Minuten erneut (sofern gerade niemand mit dem AP verbunden ist).

## Webseiten

| URL           | Funktion                                   |
|---------------|--------------------------------------------|
| `/`           | Zähler, Live-Sensorwerte, Reset-Button     |
| `/config`     | WLAN-, MQTT- und Sensor-Einstellungen (Login) |
| `/update`     | Firmware-Update über den Browser (Login)   |
| `/log`        | Konsolen-Log live im Browser, Debugmodus   |
| `/api/status` | Status als JSON                            |
| `/api/reset`  | POST: Zähler zurücksetzen                  |

Konfiguration und Update sind mit Benutzer `admin` und dem Admin-Passwort geschützt
(Standard `zaehler1`, in der Konfig änderbar). Zähler, Reset und Log sind ohne Login erreichbar.

## Update über WLAN (OTA)

Die Firmware-Version steht auf der Hauptseite und im Log.

**Im Browser:**
1. `pio run` – erzeugt `.pio/build/d1_mini/firmware.bin`
2. `http://personenzaehler.local/update` öffnen, anmelden, Datei wählen, hochladen.
3. Das Gerät startet danach automatisch neu.

**Direkt aus PlatformIO:**

```
pio run -e d1_mini_ota -t upload
```

In [platformio.ini](platformio.ini) unter `[env:d1_mini_ota]` ggf. `upload_port`
(Hostname oder IP) und `--auth` (Admin-Passwort) anpassen. Im AP-Modus ist die IP `192.168.4.1`.

Konfiguration und Zählerstand bleiben bei einem Update erhalten.

## Debugging

Alle Meldungen gehen auf die serielle Konsole (`pio device monitor`, 115200 Baud) und
gleichzeitig in einen Puffer (letzte 60 Zeilen), der unter `/log` im Browser angezeigt wird.

Immer geloggt werden: Start, Reset-Grund, Konfiguration, WLAN/AP, Sensor-Init, MQTT-Verbindung
und empfangene MQTT-Befehle sowie jeder gezählte Eintritt/Austritt.

Mit dem Häkchen **Debugmodus** auf `/log` kommen zusätzlich:
- alle 0,5 s die Distanzen beider Sensoren (`[X]` = belegt), die Messrate in Hz und die
  längste Loop-Pause. Ist die Pause deutlich über 20 ms, blockiert etwas die Sensorabfrage.
- jeder Wechsel belegt/frei pro Sensor
- pro Durchgang die Zeitdifferenz zwischen A und B beim Belegen und Freiwerden
- ignorierte Durchgänge mit Grund

Der Debugmodus wird nicht gespeichert und ist nach einem Neustart wieder aus.

## MQTT

Basis-Topic konfigurierbar (Standard `personenzaehler`):

| Topic                            | Richtung | Inhalt                                   |
|----------------------------------|----------|------------------------------------------|
| `personenzaehler/state`          | publish  | `{"inside":3,"in":10,"out":7}` (retained)|
| `personenzaehler/inside`         | publish  | Personen im Raum als Zahl (retained)     |
| `personenzaehler/reinraus`       | publish  | pro Durchgang `1` (rein) / `-1` (raus), nicht retained |
| `personenzaehler/availability`   | publish  | `online` / `offline` (LWT)               |
| `personenzaehler/reset`          | subscribe| beliebiger Payload → Zähler auf 0        |
| `personenzaehler/set`            | subscribe| Zahl → Personen im Raum setzen           |

### Mehrere Zähler kombinieren

Hat ein Raum mehrere Türen, bekommt jeder Zähler ein eigenes Basis-Topic (z. B. `buero/tuer1`,
`buero/tuer2`). Ein zentraler Zähler (Node-RED, Home Assistant, ioBroker …) abonniert
`buero/+/reinraus` und addiert einfach jeden empfangenen Wert. Da die Ereignisse nicht retained
sind, wird beim Neuverbinden nichts doppelt gezählt.

Ereignisse, die passieren, während ein Zähler keine MQTT-Verbindung hat, gehen dabei verloren.

Der Zählerstand wird höchstens einmal pro Minute ins Flash geschrieben und übersteht so
einen Neustart / Stromausfall (bis auf die letzte Minute).
