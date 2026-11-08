# Coinmates: giochi arcade cooperativi con gli amici, online

Coinmates, basato su MAME, permette di giocare in 2-4 su internet ai giochi arcade co-op, senza il ritardo dei comandi del netplay classico.

> **Stato:** funziona tra PC Linux, provato con bot e reti simulate. Il pacchetto flatpak per Steam Deck è in preparazione: questa guida verrà riscritta per quello. Se qualcosa non va, copia tutto l'output del terminale e mandalo a chi ospita.

Tutti i giocatori devono avere:
- **la stessa versione**, cioè lo stesso commit di questo ramo;
- **le stesse ROM** del gioco.

La partita controlla entrambe le cose, e in caso contrario rifiuta la connessione con il motivo. I DIP switch invece li decide chi ospita: chi si unisce prende i suoi.

## Le ROM

Le ROM non sono incluse. Le cartelle sono quelle di EmuDeck: i giochi in `Emulation/roms/arcade`, i BIOS lì oppure in `Emulation/bios/mame` o `Emulation/bios`. `Emulation` è nella home, oppure sulla scheda SD (`/run/media/...`). Alcuni giochi chiedono anche lo zip di un BIOS o di un chip, per esempio `neogeo.zip` o `qsound_hle.zip` per `dino`: se manca, MAME dice quale.

## Ospitare una partita

```
flatpak run io.github.mcerina.Coinmates host ffight
```

Sullo schermo compare il codice della stanza, per esempio `Netplay: room K7QM-3XPA`: mandalo in chat agli altri. Quando entrano, il terminale scrive `player 2 joined` e la partita parte.

Per giocare in 3 o 4: `flatpak run io.github.mcerina.Coinmates host ffight 7000 3`.

Su Linux con podman, dalla cartella del progetto: `./netplay.sh host ffight`.

## Unirsi a una partita

Con il codice che ti ha mandato chi ospita, e lo stesso gioco:

```
flatpak run io.github.mcerina.Coinmates join ffight K7QM-3XPA
```

Maiuscole e trattino non contano. Trovare la stanza richiede qualche secondo: i giocatori si cercano sulla rete DHT di BitTorrent, senza server di Coinmates. Se siete nella stessa casa vi trovate anche senza internet.

## Comandi

Ognuno usa i tasti del giocatore 1, anche se nel gioco è il giocatore 2, 3 o 4.

| Azione | Tasto |
|---|---|
| Movimento | frecce |
| Pulsanti 1, 2, 3 | Ctrl, Alt, Spazio |
| Moneta | 5 |
| Start | 1 |

Si possono cambiare dal menu di MAME (Tab), in "Input Settings". Restano salvati in `~/.var/app/io.github.mcerina.Coinmates/config/coinmates/cfg`.

Durante la partita pausa, salvataggio, caricamento, reset, frameskip e DIP switch sono disattivati. I DIP switch li sceglie chi ospita, prima di ospitare.

## Se non funziona

**`room not found`.** Dopo 5 minuti senza risposta.
- Controlla il codice e il gioco, e che chi ospita abbia ancora la finestra aperta.
- Se uno dei due vede `your router uses a different port for every destination (symmetric NAT)`, il router non permette il collegamento diretto. Succede per esempio con alcuni operatori mobili. Usate Tailscale, qui sotto.

**Senza la rete DHT**, gli indirizzi si possono scambiare a mano. Chi ospita vede `Netplay: your address: ...` e lo manda in chat. Chi si unisce lo usa al posto del codice (`join ffight 203.0.113.5:7000`) e rimanda il proprio `your address`. Chi ospita lo incolla nel terminale, uno per riga.

**Tailscale**, quando il collegamento diretto non va:
1. Installatelo entrambi da https://tailscale.com/download.
2. Chi ospita condivide il proprio computer con l'altro, dalla console di Tailscale con "Share", oppure lo invita nella propria rete.
3. Chi si unisce usa l'indirizzo Tailscale di chi ospita, che inizia con `100.`: `flatpak run io.github.mcerina.Coinmates join ffight 100.x.y.z:7000`.

**Il gioco va a scatti.** Ogni correzione di una previsione sbagliata rifà gli ultimi frame, e sui giochi più pesanti (`gdarius2`, `raystorm`, `rdft`, `rdft2`, `rfjet`, `battlera`, `legendoh`) con un ping alto non sempre ci sta in un frame. Chi ospita può aumentare il ritardo dei comandi a 2 frame, che vale per tutti: `flatpak run --env=NP_ARGS="-netplay_delay 2" io.github.mcerina.Coinmates host gdarius2`.

**`refused by the host: different MAME build`.** Le versioni di Coinmates sono diverse, e il messaggio le mostra entrambe. Aggiornate tutti all'ultima.

**`refused by the host: different ROM contents`.** Le ROM del gioco non sono identiche. Controllate entrambi con `-verifyroms`.

**`Netplay: repairing a desync`.** I due computer sono andati fuori sincrono. Il gioco si ripara da solo con un piccolo salto. Se succede spesso, manda le righe `Netplay: ... items differ: ...` del terminale.
