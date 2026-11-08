# Coinmates: giochi arcade cooperativi con gli amici, online

Coinmates, basato su MAME, permette di giocare in 2-4 su internet ai giochi arcade co-op, senza il ritardo dei comandi del netplay classico.

> **Stato:** in prova. Provato con bot e reti simulate, il pacchetto flatpak per Steam Deck e PC Linux è appena nato. Se qualcosa non va, avvialo da terminale (`flatpak run io.github.mcerina.Coinmates`) e manda tutto quello che scrive.

Tutti i giocatori devono avere:
- **la stessa versione**, cioè lo stesso commit di questo ramo;
- **le stesse ROM** del gioco.

La partita controlla entrambe le cose, e in caso contrario rifiuta la connessione con il motivo. I DIP switch invece li decide chi ospita: chi si unisce prende i suoi.

## Le ROM

Le ROM non sono incluse. Le cartelle sono quelle di EmuDeck: i giochi in `Emulation/roms/arcade`, i BIOS lì oppure in `Emulation/bios` o `Emulation/bios/mame`, i samples in `Emulation/storage/mame/samples`. `Emulation` è nella home, oppure sulla scheda SD (`/run/media/...`). Alcuni giochi chiedono anche lo zip di un BIOS o di un chip, per esempio `neogeo.zip` o `qsound_hle.zip` per `dino`: se manca, MAME dice quale.

## Giocare

Apri Coinmates: sul Deck la voce "Coinmates" aggiunta a Steam, sul PC `flatpak run io.github.mcerina.Coinmates`. Scegli il gioco, tutti lo stesso, e poi:

- **Host Online**, per ospitare. In alto compare il codice della stanza, per esempio `K7Q-M3X`: mandalo in chat agli altri. Quando sono entrati tutti quelli che aspetti, premi Start. La partita parte da sola se la stanza si riempie, cioè quando sono entrati tanti giocatori quanti ne ha il gioco. Alcuni giochi per 4 hanno un DIP switch per il cabinato a 2 o a 4 giocatori: per giocare in 3 o 4 dev'essere su 4.
- **Join Online, Room Code**, per unirti. Scrivi il codice che ti hanno mandato, poi premi A (o R2, l'Invio della tastiera di Steam). Sul Deck la tastiera si apre con Steam + X. Maiuscole e trattino non contano: il codice compare come lo vede chi ospita. Trovare la stanza richiede qualche secondo: i giocatori si cercano sulla rete DHT di BitTorrent, senza server di Coinmates. Se siete nella stessa casa vi trovate anche senza internet.
- **Play Alone**, per giocare da solo.

## Comandi

Ognuno usa i comandi del giocatore 1, anche se nel gioco è il giocatore 2, 3 o 4: la moneta e lo start vanno alla sua gettoniera e al suo start.

| Azione | Controller | Tastiera |
|---|---|---|
| Movimento | croce o stick sinistro | frecce |
| Pulsanti 1, 2, 3, 4 | X, A, B, Y | Ctrl, Alt, Spazio, Shift |
| Pulsanti 5, 6 | L1, R1 | |
| Moneta | Select | 5 |
| Start | Start | 1 |
| Menu | Select + R3 | Tab |
| Esci | Select + Start | Esc |

Sono i comandi che EmuDeck imposta per MAME, presi dalla sua configurazione. Come in EmuDeck, Select + R3 preme anche il tasto Service del cabinato: giocando da soli alcuni giochi entrano nella loro schermata di test; in rete il tasto Service è sempre spento. Si cambiano dal menu, in "Input Settings", e restano salvati in `~/.var/app/io.github.mcerina.Coinmates/config/coinmates/cfg`.

Durante la partita pausa, salvataggio, caricamento, reset, frameskip e DIP switch sono disattivati. I DIP switch li sceglie chi ospita, prima di ospitare. I comandi hanno 1 frame di ritardo (17 ms). Sui giochi più pesanti per lo Steam Deck ne hanno di più: 3 su `gdarius2`, `legendoh`, `raystorm`, `rdft`, `rdft2` e `rfjet`, 2 su `aoh`, `battlera`, `dmnfrnt`, `dragnblz`, `gunbird2`, `kov2`, `rthun2`, `s1945ii`, `s1945iii`, `soldivid` e `thndzone`.

## Se non funziona

**`room not found`.** Dopo due minuti senza risposta, nella lista dei giochi.
- Controlla il codice e il gioco, e che chi ospita abbia ancora la finestra aperta.
- Se uno dei due vede `your router uses a different port for every destination (symmetric NAT)`, il router non permette il collegamento diretto. Succede per esempio con alcuni operatori mobili. Usate Tailscale, qui sotto.

**Tailscale**, quando il collegamento diretto non va:
1. Installatelo entrambi da https://tailscale.com/download.
2. Chi ospita condivide il proprio computer con l'altro, dalla console di Tailscale con "Share", oppure lo invita nella propria rete.
3. Chi si unisce scrive, al posto del codice, l'indirizzo Tailscale di chi ospita, che inizia con `100.`.

**Il gioco va a scatti.** Ogni correzione di una previsione sbagliata rifà gli ultimi frame, e con un ping alto non sempre ci sta in un frame. Chi ospita può aumentare il ritardo dei comandi, che vale per tutti: `flatpak run --env=NP_ARGS="-netplay_delay 3" io.github.mcerina.Coinmates`.

**`refused by the host: different MAME build`.** Le versioni di Coinmates sono diverse, e il messaggio le mostra entrambe. Aggiornate tutti all'ultima.

**`refused by the host: different ROM contents`.** Le ROM del gioco non sono identiche. Controllate entrambi con `-verifyroms`.

**`Netplay: repairing a desync`.** I due computer sono andati fuori sincrono. Il gioco si ripara da solo con un piccolo salto. Se succede spesso, manda le righe `Netplay: ... items differ: ...` del terminale.
