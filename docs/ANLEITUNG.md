# ExtBridge – Gebrauchsanweisung

ExtBridge macht Linux-Festplatten (ext2, ext3, ext4) auf dem Mac ganz normal benutzbar:
Die Platte erscheint im Finder wie jede andere externe Festplatte – zum Lesen **und** Schreiben.

*English version: [USER-GUIDE.md](USER-GUIDE.md)*

## Voraussetzungen

* macOS 15.4 oder neuer (Apple-Chip oder Intel)
* eine Festplatte, ein USB-Stick oder eine SD-Karte mit einem Linux-Dateisystem (ext2/ext3/ext4)

## Installieren

1. Lade die neueste `ExtBridge-….dmg` von der
   [Release-Seite](https://github.com/schnierk/ExtBridge/releases/latest) herunter und öffne sie.
2. Ziehe **ExtBridge** in den Ordner **Programme**.
3. Öffne ExtBridge aus dem Ordner Programme.
4. Die App zeigt „Fast geschafft!“. Klicke auf **Systemeinstellungen öffnen**, wähle
   **Nach Kategorie → Dateisystem-Erweiterungen** (ⓘ) und schalte **ExtBridge ext4** ein.
   Das ist nur einmal nötig.
5. Steck deine Linux-Platte an. Sie erscheint im Finder und in der App.

## Benutzen

* Im **Finder** arbeitest du mit der Platte wie mit jeder anderen: öffnen, kopieren, umbenennen,
  löschen.
* **Vor dem Abziehen immer auswerfen** (Finder: ⏏ neben dem Namen, oder in der App).
* Die **App** zeigt für jede Platte:
  * **Übersicht** – Belegung, Zustand, Journal, Modus (nur lesen oder lesen & schreiben)
  * **Diagnose** – Prüfung des Dateisystems (verändert nichts) und Geschwindigkeitstest
  * **Informationen** – alle technischen Daten
  * **Einstellungen** – *Immer nur lesend einhängen*, *Nicht automatisch einhängen*
* In der **Menüleiste** gibt es ein kleines Laufwerk-Symbol zum schnellen Einhängen und Auswerfen.
* Unter **ExtBridge → Einstellungen** (⌘ ,) legst du fest, wem neue Dateien gehören
  (Linux-Benutzer ↔ Mac-Benutzer) und ob Mac-Hilfsdateien wie `.DS_Store` auf der Platte landen
  dürfen. Dort kannst du ExtBridge auch **beim Anmelden** starten lassen.

## Sicherheit

* Jede Änderung geht zuerst ins **Journal** – genau wie unter Linux. Auch wenn der Strom ausfällt
  oder das Kabel gezogen wird, bleibt das Dateisystem in Ordnung.
* Bevor macOS eine Platte einhängt, prüft ExtBridge sie. Findet die Prüfung Fehler, wird die Platte
  **nur lesend** eingehängt: Du kommst an deine Dateien, aber es wird nichts verändert. Repariert
  wird so eine Platte unter Linux mit `e2fsck -f`.
* Platten mit Linux-Funktionen, die ExtBridge nur lesen kann, werden ebenfalls nur lesend
  eingehängt. Die App erklärt jeweils, warum.
* Für besonders wertvolle Sammlungen: in der App *Immer nur lesend einhängen* einschalten.
* ExtBridge ist eine **Beta-Version**. Bitte hab von wichtigen Daten immer eine Sicherung.

## Wenn etwas nicht klappt

* **Die Platte erscheint nicht:** In der App unten links muss „Treiber aktiv“ stehen. Sonst die
  Erweiterung wie oben bei Schritt 4 einschalten.
* **Die Platte steckte schon beim Einschalten des Macs:** macOS prüft sie dann, bevor ExtBridge
  läuft. Steck sie nach der Anmeldung einmal ab und wieder an. Die App zeigt das selbst an.
* **Nach einem Update** von ExtBridge ist die Erweiterung manchmal wieder aus – dann wie bei der
  Installation wieder einschalten.
* **„Platte wird nicht eingehängt, Mac neu starten“:** Wenn macOS sich ein altes Einhängen gemerkt
  hat, hilft nur ein Neustart des Macs. Die App zeigt das an.
* **Protokoll** (für Fehlermeldungen, im Terminal):
  `log stream --predicate 'subsystem == "de.nexami.extbridge"' --level info`

Fehler melden: [Issues](https://github.com/schnierk/ExtBridge/issues)

## Unterstützen

ExtBridge ist kostenlos und quelloffen. Wenn es dir hilft, freue ich mich über einen Kaffee –
über den Knopf in der App oder direkt:
[PayPal](https://paypal.me/popierk) · [Karte / Apple Pay](https://buy.stripe.com/aFa00l8Rm1Hl5wbauk1B601)
