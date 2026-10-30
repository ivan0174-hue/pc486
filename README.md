# pc486 — PC 486 emulato su Raspberry Pi 4 (bare metal)

pc486 emula un PC 486 dei primi anni '90 e gira direttamente sull'hardware del
Raspberry Pi 4 / Pi 400, senza Linux. Si accende dalla scheda SD in pochi
secondi ed esegue il **BIOS originale** della scheda madre e quello della
scheda video.

## Hardware emulato

| Componente | Emulazione |
|---|---|
| CPU | 486 con FPU (interprete derivato da tiny386) |
| Scheda madre | BIOS AMI 04/04/93, chipset **ALi M1429** (shadow RAM C0000–FFFFF) |
| RAM | 4–64 MB (16 MB predefiniti) |
| Video | **S3 ViRGE 86C325** (Diamond Stealth 3D 2000), 4 MB, VGA completa + modi VESA 1.2 |
| Dischi | 2 canali IDE (dischi fissi e CD-ROM ATAPI da file immagine) |
| Floppy | controller Intel 82077, unità da 360K a 2,88M (immagini .img) |
| Tastiera | controller 8042 compatibile AMI "MegaKey", tastiera USB |
| Mouse | seriale Microsoft su COM1 (oppure PS/2), mouse USB |
| Audio | Sound Blaster 16 + AdLib (OPL2) + PC speaker, su jack 3,5 mm o HDMI |
| Altro | PIC 8259, PIT 8254, DMA 8237, orologio/CMOS MC146818 salvato su SD, COM1/COM2 |

## Preparare la scheda SD

1. Formatta una scheda microSD in **FAT32** (le schede fino a 32 GB vanno bene così come sono).
2. Copia **tutto il contenuto** della cartella `sdcard/` nella radice della scheda:
   ```
   config.txt  cmdline.txt  kernel8-rpi4.img  armstub8-rpi4.bin
   start4.elf  fixup4.dat   bcm2711-*.dtb      LICENCE.broadcom
   pc486/      (BIOS, configurazione, immagini disco)
   ```
3. Inserisci la scheda nel Pi 4, collega monitor HDMI (porta HDMI0, quella vicina
   all'alimentazione), tastiera e mouse USB, e accendi.

Al primo avvio la CMOS viene creata con le impostazioni predefinite del BIOS AMI
già valide (floppy A: 1,44 MB, disco C: tipo 47 con la geometria dell'immagine,
avvio A: poi C:). Le modifiche fatte nel setup del BIOS vengono salvate in
`pc486/cmos.bin`.

Il pacchetto include un disco fisso da 100 MB con **FreeDOS** già installato
(`freedos_hd.img`, con HIMEM e driver del mouse), così il PC parte subito al
prompt `C:\>`.

## Tasti

| Tasti | Funzione |
|---|---|
| **Ctrl+Alt+F12** | menu dell'emulatore: cambio floppy/CD, velocità, reset, salva CMOS, riavvio del Pi |
| **Ctrl+Alt+F11** | pulsante "turbo": rallenta la CPU a un 386SX-16 e la riporta alla velocità di prima (in alto compare un avviso per 2 secondi) |
| **Canc** (tenuto premuto all'accensione) | entra nel setup del BIOS AMI |
| Ctrl+Alt+Canc | riavvio "a caldo" del PC emulato |

Nel menu: frecce su/giù per scegliere, Invio per confermare, Esc per uscire;
sulla voce *Velocità* le frecce sinistra/destra cambiano il limite.

## Configurazione: `pc486/pc486.ini`

```ini
[pc]
mem_size = 16M          ; RAM emulata
hda = freedos_hd.img    ; disco fisso primario (master)
;hdb = dati.img         ; secondo disco
;cdc = gioco.iso        ; CD-ROM sul canale secondario
;fda = disco1.img       ; floppy inserito all'avvio
sound = 1
serial_mouse = 1        ; 0 = mouse PS/2 (per Windows 95 / driver PS/2)
pci = 1                 ; bus PCI con la ViRGE (servizi PCI BIOS)

[cpu]
speed = 0               ; 0 = massima, altrimenti milioni di istruzioni al
                        ; secondo (anche decimali): 40 ≈ 486DX2-66,
                        ; 20 ≈ 486DX-33, 2 = 386SX-16

[display]
aspect = 4:3            ; oppure stretch

[sound]
sound_out = both        ; both = HDMI e jack insieme, pwm = solo jack,
                        ; hdmi = solo HDMI, none = muto
```

Tutte le immagini (`.img`, `.ima`, `.iso`) messe nella cartella `pc486/` appaiono
nel menu Ctrl+Alt+F12.

### Creare nuovi dischi

* **Disco fisso vuoto**: crea un file di dimensione multipla di 516096 byte
  (16 testine × 63 settori × 512), al massimo 504 MB per questo BIOS del 1993.
  Esempio da Linux/macOS: `truncate -s 200M nuovo.img`. Poi, dal PC emulato:
  `FDISK`, riavvio, `FORMAT C: /S` (o `SYS C:`).
* **Floppy con dei file**: `python3 tools/mkfloppy.py disco.img FILE1.EXE FILE2.DAT`
  crea un floppy FAT12 da 1,44 MB da usare con il menu.

* **Disco dati con dei file dal PC** (per esempio i dischetti di installazione
  di Windows copiati in una cartella):
  `python3 tools/mkhdd.py dati.img 200 cartella/` crea un disco FAT16 da 200 MB
  con dentro tutta la cartella (sottocartelle comprese, nomi lunghi accorciati
  in 8.3). Mettilo in `pc486/` e aggiungi `hdb = dati.img` in `pc486.ini`:
  in DOS diventa l'unità **D:**.

Le immagini create da altri emulatori (86Box, PCem, DOSBox-X, QEMU *raw*) sono
compatibili.

## Usare una partizione della SD come disco C:

Invece di un file immagine, il disco fisso del PC emulato può essere una
**partizione vera della scheda SD**. Il DOS la formatta in FAT16 e, quando
rimetti la SD nel computer, vedi i file del DOS direttamente, senza strumenti.

La scheda deve avere due partizioni: la prima FAT32 per il Raspberry (con i
file di `sdcard/`), la seconda da **massimo 500 MB** per il DOS (il BIOS AMI del
1993 non va oltre 504 MB).

**Da Windows** (attenzione: `clean` cancella tutta la SD, scegli il disco giusto):
```
diskpart
list disk
select disk N                      (N = la scheda SD)
clean
create partition primary size=2048
format fs=fat32 quick label=PI486
active
assign
create partition primary size=500
set id=06 override
exit
```
Copia il contenuto di `sdcard/` nella prima partizione. Se Windows chiede di
formattare la seconda, rispondi **Annulla**: la formatta il DOS.

**Da Linux** (`/dev/sdX` = la scheda SD):
```
sudo parted /dev/sdX --script mklabel msdos \
     mkpart primary fat32 1MiB 2049MiB mkpart primary fat16 2049MiB 2549MiB
sudo mkfs.vfat -F 32 -n PI486 /dev/sdX1
```

Poi in `pc486.ini`:
```ini
hda = sd:2        ; disco C: = partizione 2 della SD
```
Si possono usare anche `sd:3` e `sd:4`, e come secondo disco (`hdb = sd:3`).
Al primo avvio pc486 imposta la CMOS, e l'installazione del DOS vede un disco
C: con la partizione già pronta da formattare. **Non usare FDISK** su questo
disco: la tabella delle partizioni vista dal DOS è fissa (la gestisce
l'emulatore) e le modifiche verrebbero ignorate.

## Installare MS-DOS 6.x

1. Metti le immagini dei dischetti di installazione (es. `dos622_1.img`,
   `dos622_2.img`, `dos622_3.img`, da 1,44 MB) nella cartella `pc486/`.
2. Crea un disco fisso vuoto, ad esempio da 200 MB:
   `truncate -s 200M dos.img` (Linux/macOS) oppure
   `fsutil file createnew dos.img 209715200` (Windows), e mettilo in `pc486/`.
3. In `pc486.ini` scrivi `hda = dos.img` (oppure `hda = sd:2`, vedi sopra)
   e `fda = dos622_1.img`.
   All'avvio pc486 aggiorna da solo la CMOS con la geometria del nuovo disco.
4. Il PC parte dal dischetto e lancia il programma di installazione: lascia
   fare a lui partizione e formattazione. Quando chiede il disco successivo,
   premi **Ctrl+Alt+F12**, scegli *Floppy A:* e l'immagine seguente.
5. Alla fine togli `fda` da `pc486.ini` (o espelli il dischetto dal menu).

Per tenere anche FreeDOS, usa il disco DOS come `hda` e `freedos_hd.img` come `hdb`.

## Windows 3.1

L'emulatore esegue Windows 3.1 anche in **modalità 386 avanzata** (modo
protetto, paginazione, V86: verificati con EMM386). Serve una copia tua di
Windows 3.1 e, consigliato, di MS-DOS 6.22 (con FreeDOS Windows 3.1 funziona
in modo meno affidabile).

1. **DOS**: installa MS-DOS sul disco C: (dischetti `.img` nella cartella
   `pc486/`, cambiati con Ctrl+Alt+F12), oppure parti dal disco FreeDOS incluso.
2. **File di installazione**: o metti le immagini dei dischetti di Windows in
   `pc486/` e lancia `A:\SETUP` cambiando i dischi dal menu, oppure copiali
   tutti in una cartella, crea un disco con `tools/mkhdd.py` e lancia
   `D:\SETUP` (più comodo: nessun cambio di dischi).
3. **Durante l'installazione** scegli:
   * Video: **VGA** (per partire sul sicuro);
   * Mouse: **Microsoft o IBM PS/2** (funziona anche col mouse seriale
     emulato su COM1);
   * Accesso a 32 bit al disco: **no**.
4. **Driver video S3**: dopo, installa il driver Windows 3.1 della Diamond
   Stealth 3D 2000 / S3 ViRGE (dal disco driver della scheda) con *Windows Setup*
   → *Modifica impostazioni di sistema* → *Schermo* → *Altro*. La ViRGE è
   visibile sul bus PCI emulato e l'accelerazione 2D è emulata. Se Windows non
   parte col driver S3, da DOS esegui `SETUP` nella cartella `C:\WINDOWS` e
   rimetti **VGA**.
5. **Audio**: driver Sound Blaster 16, indirizzo 220, IRQ 5, DMA 1, DMA alto 5.

Se Windows va troppo veloce o certi programmi danno errori di temporizzazione,
abbassa la velocità dal menu (voce *Velocità'*).

## Giochi

* **Giochi vecchi che vanno troppo veloci o si fermano con un errore**
  (per esempio *run-time error R6003 - integer divide by 0* in The Secret of
  Monkey Island, o *Runtime error 200* nei programmi Turbo Pascal): all'avvio
  misurano la velocità della CPU e su una macchina veloce il conto va fuori
  scala, come succedeva sui 486 veri. Premi **Ctrl+Alt+F11** (turbo spento)
  *prima* di lanciare il gioco; quando il gioco è partito puoi ripremerlo per
  tornare alla velocità piena, perché la misura viene fatta una volta sola.
* **Audio**: la Sound Blaster è a 220h, IRQ 5, DMA 1 (16 bit: DMA 5) e l'AdLib
  a 388h. Molti giochi la trovano solo se in `AUTOEXEC.BAT` c'è
  `SET BLASTER=A220 I5 D1 H5 T6`; negli altri va scelta nel programma di
  setup del gioco (spesso `SETUP.EXE` o `INSTALL.EXE`). L'audio esce sia
  dall'HDMI sia dal jack (`sound_out = both`).

## Limiti noti di questa versione

* **Accelerazione 2D della ViRGE**: emulati BitBLT, riempimenti, linee e
  poligoni (tutte le 256 ROP, pattern mono/colore, espansione monocromatica,
  dati dalla CPU, clipping) a 8, 16 e 24 bit. Il motore 3D (S3D) non è
  emulato.
* **Bus PCI**: la scheda madre originale non ce l'ha; pc486 aggiunge un ponte
  PCI ALi M1435 con la ViRGE e un'estensione BIOS con i servizi PCI (INT 1Ah).
  Si disattiva con `pci = 0` in `pc486.ini`.
* Il BIOS mostra "16MHz CPU Clock": misura la CPU cronometrando un'istruzione,
  cosa che un interprete non riproduce. È solo un'indicazione: la velocità reale
  dipende dal Pi e dall'opzione `speed`.
* Niente rete, niente porta parallela, niente joystick.
* Il Pi 4 non ha orologio: data e ora del PC emulato ripartono da quelle salvate
  nella CMOS; impostale da DOS con `DATE` e `TIME`.

## Diagnostica

Se lo schermo resta nero, il kernel scrive un log sulla seriale del Pi
(GPIO 14/15, 115200 baud). Messaggi d'errore come BIOS mancante o SD non
leggibile compaiono anche sullo schermo.

## Compilare dai sorgenti

Serve un computer Linux con `clang` (≥ 15), `lld`, `llvm-objcopy`, `git`, `make`.

```sh
./tools/build_pi.sh          # scarica Circle, compila, crea dist/sdcard
make                         # versione di test per Linux: ./pc486_headless
```

`pc486_headless` esegue la stessa macchina su Linux senza finestra: utile per
provare configurazioni (`-t secondi`, `-T 5:dir~n` per digitare testo, `-s 10:schermata.ppm`,
`-d 10` per stampare lo schermo di testo, `-p` per i codici POST).

## Struttura

```
core/    emulatore (C99, portabile): CPU, chipset, periferiche, S3 ViRGE, menu
pi/      frontend Raspberry Pi 4 su Circle (C++), mini libc per il bare metal
linux/   frontend di test per Linux
tools/   mkfloppy.py, mkhdd.py, build_pi.sh, test (vtest, atest, pcitest), MBR, PCI BIOS
```

Vedi `NOTICE.md` per le licenze dei componenti di terze parti.
