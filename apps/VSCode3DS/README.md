# VSCode3DS

Il programma è contenuto interamente in `VSCode3DS.c`. Lo stesso file produce l'app AuroraOS e il servizio di compilazione per macOS/Linux, scegliendo la modalità durante la compilazione. Non servono altri sorgenti specifici di questo progetto.

È incluso anche `VSCode3DS.bin`, già compilato per AuroraOS. Il file sorgente contiene inoltre una modalità di test del motore di modifica.

## Versione verificata

Repository AuroraOS: [TeamAuroraOS/AuroraOS](https://github.com/TeamAuroraOS/AuroraOS), commit [`8123fd90b606e5a1c44d8a7e474edce83681dcab`](https://github.com/TeamAuroraOS/AuroraOS/tree/8123fd90b606e5a1c44d8a7e474edce83681dcab). Il relativo README indica **Beta 7 v0.1.6**, e `aurora_app.h` dichiara **C SDK 1.2**. La precedente ricognizione della chat si riferiva a una versione anteriore.

La compilazione ARM e quella del servizio sono state eseguite con gli avvisi trattati come errori. Non è stato eseguito un test su console fisica: touch, compatibilità dei modelli, prestazioni, uscita HOME e Wi-Fi devono ancora essere collaudati su hardware.

## Installare l'app

1. Usa AuroraOS Beta 7 con il relativo runtime e una SD funzionante.
2. Copia `VSCode3DS.bin` in `SD:/Aurora/Apps/C/`.
3. Avvialo dal menu di AuroraOS. La sua cartella dati sarà `SD:/Aurora/Apps/C/VSCode3DS/`.

Il progetto iniziale si trova in `SD:/Aurora/Apps/C/VSCode3DS/projects/Default/`. Puoi scegliere un'altra cartella SD dal comando **Project folder**. Salvare tutti i documenti è richiesto prima di cambiare progetto.

Non avviare l'IDE rinominandolo `AURORAOS.BIN`: usa il menu di AuroraOS, che fornisce i servizi ARM11 necessari a touch e rete.

## Ricompilare l'app dal singolo C

Installa devkitARM e Python 3. Scarica la repository AuroraOS verificata, poi dalla sua cartella principale esegui:

```sh
python3 sdk/aurcc.py build /percorso/VSCode3DS.c -o VSCode3DS.bin
```

Su Windows il launcher può essere `py` al posto di `python3`. Il servizio host qui fornito è per macOS/Linux.

## Preparare il computer per le build

Il computer deve avere la repository AuroraOS completa, Python 3.10 o successivo e devkitARM. `arm-none-eabi-gcc` e `arm-none-eabi-objcopy` devono essere nel `PATH` ereditato dal servizio.

Compila il servizio usando lo stesso file:

```sh
cc -std=c11 -O2 -Wall -Wextra -DVS3DS_HOST VSCode3DS.c -o vs3ds-host
```

Avvialo indicando la cartella principale di AuroraOS, un token di 16–95 caratteri e l'IP LAN del computer:

```sh
./vs3ds-host --root /percorso/AuroraOS --token IL_TUO_TOKEN_LUNGO --bind 192.168.1.2
```

L'IP è un esempio: inserisci quello reale del computer. La porta predefinita è `45321`; puoi cambiarla con `--port`. Senza `--bind`, il servizio ascolta solo su `127.0.0.1` e il 3DS non può raggiungerlo. Se il firewall chiede l'accesso, consentilo sulla rete privata che userai.

Il trasporto è TCP in chiaro, per compatibilità con AuroraOS. Usa una LAN fidata; non aprire questa porta su Internet. Il token autentica le richieste ma non cifra token e sorgenti. Le somme di controllo FNV verificano errori di trasferimento, non forniscono autenticità crittografica.

Sul 3DS configura la rete Wi-Fi nelle impostazioni di AuroraOS. In VSCode3DS, premi **START** e imposta **Host address**, **Host token**, **Host port**, poi usa **Test connection**. Il computer deve restare acceso e il servizio in esecuzione.

## Progetti Auric e C

Dal menu puoi creare file `.aur`, `.c`, `.h` e altri file di testo. Imposta **Project entry file** sul sorgente principale, ad esempio `main.aur` oppure `main.c`. Imposta **Output app name** per il nome dell'app installata.

- **Auric:** il servizio compila il file principale con `compiler.aurc` e produce un contenitore `AUR1`.
- **C:** il servizio compila ricorsivamente i file `.c` della cartella progetto con `sdk/aurcc.py`, usando la cartella progetto anche come include path, e produce `AURC`.
- Tieni programmi C indipendenti in cartelle progetto distinte: più funzioni `main` nello stesso progetto causano un errore di collegamento.
- Un file `app.icon` alla radice del progetto fornisce l'icona testuale 32×32 del menu.
- Il contenuto di `assets/` viene copiato nella cartella dati dell'app installata, mantenendo le sottocartelle. In C usa `app_dir()` per costruirne i percorsi. In Auric puoi usare i percorsi relativi previsti dal suo runtime.
- I nomi relativi dei progetti devono usare caratteri ASCII stampabili, senza componenti nascosti o riservati; i contenuti dei documenti possono essere UTF-8.

Premi **Y** oppure **Build**. Il progetto viene salvato e trasferito a blocchi, verificato, compilato e scaricato. Puoi continuare a modificare i buffer, ma i salvataggi e le operazioni che cambiano i file rimangono bloccati durante la build per mantenere coerente il progetto su disco. Non modificare la SD da un altro dispositivo durante la build.

I trasferimenti supportano la ritrasmissione dei blocchi. Dopo errori di trasporto il client riprova e poi permette di riprendere con **A** oppure annullare con **B** nel pannello build. Una singola chiamata del SDK può bloccare temporaneamente input e rete: la cancellazione non interrompe quella chiamata a metà.

Le app vengono installate in `SD:/Aurora/Apps/` per Auric e `SD:/Aurora/Apps/C/` per C. Puoi scegliere tra sostituzione con backup `.bak` e copie numerate. Avvia il risultato dal menu Home di AuroraOS; il programma non inventa un'API di lancio diretto non presente nel SDK pubblico.

## Controlli e interfaccia

| Controllo | Azione |
|---|---|
| D-pad | Muove il cursore; tenendolo premuto ripete |
| A + D-pad | Seleziona testo |
| B | Backspace, con ripetizione nell'editor; annulla nelle finestre |
| X | Salva il documento |
| Y | Compila il progetto |
| L / R | Documento precedente / successivo |
| START | Apre o chiude l'elenco completo dei comandi |
| SELECT | Cambia la pagina della tastiera |
| SELECT + A | Seleziona tutto |
| SELECT + sinistra / destra | Undo / redo |
| SELECT + X / Y | Copia / incolla |
| SELECT + su / giù | Pagina precedente / successiva |
| Circle pad | Scorre il testo senza muovere il cursore |

La combinazione di sistema **L + giù + SELECT** appartiene ad AuroraOS.

Lo schermo superiore mostra il codice. Lo schermo inferiore offre tastiera, simboli e snippet, esploratore, ricerca, diagnostica e guida offline. **Bottom code / keyboard** abilita una vista inferiore del testo: puoi toccare una posizione e fare doppio tocco per selezionare una parola.

Le voci di menu includono ricerca avanti/indietro, maiuscole/minuscole, parole intere, sostituzione singola e globale, vai a riga, navigazione per parola, indentazione, commenti, duplicazione/spostamento/eliminazione righe, selezioni e salvataggio con nome. **Replace current matching selection** sostituisce un risultato selezionato da **Find**.

**Complete identifier** propone nomi dalla guida incorporata e dai documenti aperti. La guida include l'header SDK pubblico e il riferimento Auric completo dello snapshot verificato. Puoi cambiare dimensione del testo, tema, a capo visivo, tabulazioni, indentazione, coppie automatiche e terminatori LF/CRLF.

L'esploratore legge pagine di 256 voci; con L/R o i comandi di pagina accedi alle altre. Le voci sono ordinate all'interno di ciascuna pagina. Puoi rinominare anche file aperti e cartelle che li contengono. L'eliminazione richiede una conferma testuale, richiede di chiudere i documenti interessati e rimuove solo cartelle vuote. FAT non offre un cestino.

## Recupero e limiti

La sessione viene salvata ogni 15 secondi e all'uscita gestita dal runtime, inclusa HOME. Il recupero conserva i buffer non salvati separatamente dai file originali. La sessione ha una somma di controllo e un backup. Solo l'ultimo progetto ha ripristino automatico; i recenti sono conservati per l'ultimo progetto.

Il salvataggio usa temporaneo e backup. Un'interruzione dell'alimentazione può comunque interrompere una sequenza di operazioni FAT: se serve, controlla `.tmp` e `.bak`. Un'interruzione improvvisa può perdere le modifiche successive all'ultimo checkpoint riuscito.

| Risorsa | Limite |
|---|---|
| Documenti aperti | 6 |
| Documento / file trasferito | 512 KiB; al massimo 65.536 righe per documento |
| Undo/redo | 64 modifiche raggruppate per documento, budget globale 2 MiB |
| Progetto trasferito | 64 file, 4 MiB complessivi, profondità 8 |
| Percorso relativo | 159 byte |
| Percorso SD completo | Meno di 256 byte, secondo il SDK |
| Output compilato | 15 MiB |
| Log sul 3DS | 64 KiB; log completo sul computer |
| Compilazioni contemporanee | 1; massimo 180 secondi per processo di build |

Un file CRLF che supera 512 KiB dopo la conversione viene rifiutato al salvataggio: scegli LF o dividi il documento. UTF-8 viene conservato e navigato per codepoint, ma il font monospace mostra `?` per i glifi non supportati. Non c'è un IME Unicode.

La colorazione e i completamenti sono lessicali. Il compilatore sul computer fornisce gli errori semantici Auric/C. Non sono inclusi un language server completo, Git, espressioni regolari o un debugger interattivo. L'IDE non compila C o Auric autonomamente sulla console.

I log e le cartelle di lavoro restano in `AuroraOS/.vs3ds/<sessione>/` sul computer. Il servizio non elimina automaticamente quelle cartelle: puoi rimuovere quelle vecchie a servizio fermo. Riavviare il servizio azzera le sessioni in memoria.

## Verifiche eseguite

- Compilazione del servizio macOS con `-Wall -Wextra -Werror`.
- Compilazione effettiva ARM del singolo sorgente tramite SDK 1.2 con `-Werror`.
- Test del motore più 10.000 casi casuali di modifica, undo, redo e indice delle righe, con AddressSanitizer e UndefinedBehaviorSanitizer.
- Test dei comandi con un SDK simulato: parentesi, indentazione, sostituzioni/ricerche adiacenti, spostamento e commento righe, salvataggio e recupero, rifiuto di sessioni corrotte e diagnostica Auric/C.
- Protocollo TCP su localhost con compilatori reali Auric e C: autenticazione, upload frammentati e ripetuti, checksum, header multipli di progetto C, file vuoti, log e download a blocchi.
- Rifiuto di percorsi fuori progetto, build incomplete, modifiche a una build già avviata e download dopo errori; annullamento del processo di compilazione.
- Compilazione del template Auric generato dall'editor.

Il test integrato del motore si esegue dallo stesso file:

```sh
cc -std=c11 -g -DVS3DS_TEST VSCode3DS.c -o vs3ds-test
./vs3ds-test
```

Il collaudo finale su Old/New 3DS rimane necessario. Non sono stati misurati frame rate o prestazioni su hardware.

## Provenienza

Il sorgente contiene il riferimento SDK/Auric del commit indicato e richiami alle licenze upstream. È marcato `GPL-3.0-or-later`; per distribuire l'app conserva anche gli avvisi e gli obblighi delle componenti AuroraOS collegate, descritti in [SDK — Licences](https://github.com/TeamAuroraOS/AuroraOS/blob/8123fd90b606e5a1c44d8a7e474edce83681dcab/sdk/README.md#licences) e nella repository originale.
