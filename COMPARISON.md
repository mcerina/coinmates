# Why Coinmates: compared with RetroArch and Fightcade

Arcade games online already exist: RetroArch has netplay, and Fightcade is where people play arcade fighting games against each other. So why another one? Because Coinmates is made for one thing they don't aim at: **2 to 4 friends playing the co-op classics together, from far away, with nothing to set up**, on Linux and the Steam Deck as on Windows and Mac. This page says, as honestly as we can, where Coinmates does better and where it does worse; we think the first list matters more for playing with friends, but judge for yourself.

The numbers come from tests on one machine, with the same simulated networks and the same bot playing in all three (how, [at the end](#how-we-measured)): Coinmates 1.4.0, RetroArch 1.22.2 with the FBNeo core set up as [EmuDeck](https://www.emudeck.com/) sets it up, and Fightcade 2.1.45.

## In short

| | Coinmates | RetroArch + FBNeo | Fightcade |
|---|---|---|---|
| players | 2–4 | 2 or more, and spectators | 2, and spectators |
| finding each other | a room code, nothing else | an IP address and an open port, or a public lobby | an account on Fightcade's servers |
| home routers | nothing to open | a port to open (UPnP or by hand), or a relay | handled by Fightcade |
| both players see the same game | checked all the time, and repaired | checked, but the check fails all the time with FBNeo, so the game is reloaded every 10 seconds | not checked: in half the games the players drift apart, mostly in the sound |
| rollback and time sync | yes, both | rollback, no time sync | yes, both (GGPO) |
| a player who drops out | the others play on; they can join again | the others play on | the match ends |
| the host leaves | the next player hosts | the game ends | (no host: the match ends) |
| Linux and Steam Deck | native | native, in EmuDeck | a Windows program under Wine or Proton |
| games | 256 co-op games, each tested for netplay | all of FBNeo, thousands, none tested for netplay | Fightcade's catalog, made for 2 players: mostly fighting games, on FBNeo and other emulators |
| extras | — | RetroAchievements (softcore in netplay), every RetroArch core | rankings, replays, a large community |
| open source | all of it (GPL) | all of it (GPL) | the emulator; the netplay library and the client are closed |

## Where Coinmates does better

**Everybody sees the same game, and Coinmates is the only one of the three whose check works.** Rollback netplay only works if the emulator, going back to a saved state and playing the same inputs again, ends up exactly where the other players are, down to the last bit. Coinmates compares a hash of the whole state of the game (processors, sound, video) every half second, and if the players differ, sends the right state. To get there, we fixed the saved states of many of MAME's drivers, game by game, and every game in Coinmates passed a sync test and online tests before being listed.

- **RetroArch with FBNeo:** we played all the 252 games that Coinmates and FBNeo have in common, with RetroArch's check turned off, to see whether the players stay together on their own. In 41 games the game's memory differs between the players within 40 seconds. In Coinmates: none of the 252. RetroArch has its own check (every 10 seconds, with EmuDeck's settings), but with FBNeo it fails every time, even when the players are together, so it reloads the host's state every 10 seconds: a hitch, and 3 MB each time for a large game like Raiden Fighters. And reloading doesn't always help: in Teenage Mutant Ninja Turtles the players differ from the moment the second one joins, reload after reload.
- **Fightcade** has no check: its checksum is computed only in debug builds, and is always 0 in the releases. Its emulator ran 213 of the same 252 games with our ROM files (it lacks some, wants different files for others, and a few stopped). In 106 of them the two players' states differ within 40 seconds. Mostly it is the sound chips (62 games) or the video chips (16), which players hardly notice. But in at least 14 games the same memory that we compared in RetroArch differs, and in 5 a processor. In 9 more, the difference didn't show again when we played them a second time. Nobody is told, and nothing repairs it.

**Shorter corrections than RetroArch.** RetroArch's netplay doesn't keep the players' clocks together: the host runs ahead and redoes, each time, a whole round trip of the other player's inputs. Italy–Australia (300 ms round trip), the host's rollbacks are 18–24 frames on average in RetroArch, 6 in Coinmates; overall RetroArch redoes about 3 times as many frames, at every distance, which you see as characters jumping back. Fightcade keeps the clocks together like Coinmates, and with the same input delay redoes about as much.

**No input delay on a LAN or a good connection.** Up to about 100 ms of round trip (anywhere in Europe), Coinmates plays most games with no input delay, like RetroArch. Further away it adds a little, by itself, during the game: see the weak points.

**Bad connections don't end the game.**

| | Coinmates | RetroArch | Fightcade |
|---|---|---|---|
| Italy–Australia between two phones (600 ms, 2% lost) | stops 0.7 s a minute | stops 4–6 s a minute; with a large game, the session fell in 2 tries out of 3 | doesn't stop, but corrects deeper (see below) |
| 12 seconds without network | after 5.6 s each side plays on; when the network is back, the game joins up again by itself | the players are thrown out | everybody waits, then the match goes on |
| a player freezes for 15 s | after 5.6 s the others play on; the frozen one joins again by itself | the frozen one is thrown out | the other waits the whole 15 s, then the match goes on |
| a player's game crashes | after 5.6 s the others play on; the player can join again | the others play on | the match ends |
| the host's game crashes | after 5.6 s the others play on, with a new host | the game ends for everybody | the match ends |

**Nothing to set up, nobody to trust.** No account, no server of ours or anybody's to run: the host gets a room code, the players find each other through the public BitTorrent trackers and connect directly, with no port to open on the router. Only who has the code can join. RetroArch needs an open port (often not possible behind a phone's or a provider's shared address), or a relay, which EmuDeck sets in New York; and EmuDeck announces the game in RetroArch's public lobby, with no password. EmuDeck's own NetPlay tool works only at home. Fightcade needs an account on its servers.

**Up to 4 players**, for the beat 'em ups made for 4. Fightcade has 2.

**Native on Linux and the Steam Deck**, with EmuDeck's folders and controls. Fightcade is a 32-bit Windows program: on Linux and the Deck it runs under Wine (which its Flathub package includes) or Proton.

## Where Coinmates does worse

**MAME is slower than FBNeo.** On the same games, FBNeo runs 1.1 to 1.7 times faster on the older boards (CPS1, CPS2, Teenage Mutant Ninja Turtles), and 3 to 8 times faster on the heavier ones (Neo Geo, Seibu SPI, Psikyo, Cave). Each frame redone costs a frame of emulation, so on a heavy game Coinmates can redo fewer frames, and adds more input delay instead: on Raiden Fighters, 2 to 3 frames on home wifi, where the other games have almost none. RetroArch and Fightcade, with FBNeo, don't pay this.

**Far away, more input delay.** The further, the more Coinmates waits for the others' inputs instead of correcting the game, a deliberate trade for fewer and shorter corrections on screen: about 3 frames (50 ms) at a 300 ms round trip, 13 frames (215 ms) at 600 ms between two phones on opposite sides of the world. RetroArch stays at 0 and corrects up to a whole second of game on screen. Fightcade's delay is chosen before the match and stays the same.

**Very far away, short stops.** Only at the far end of the world: up to 100 ms of round trip Coinmates never stops. It never predicts more than 9 frames ahead; when an input comes later than that, the game stops for a frame or two. At a 600 ms round trip that is about 30 times a minute, 0.7 seconds in all. With the same input delay Fightcade doesn't stop: it predicts up to about 20 frames, and corrects up to 14 frames instead, which you see as a jump. Both are visible; FBNeo's speed lets Fightcade afford the deeper corrections. At 300 ms, Coinmates stops about 0.1 seconds a minute.

**A player who vanishes holds the game for 5.6 seconds.** If someone's game crashes, the power goes, or the connection drops, the others play on only 5.6 seconds after the last word from that player: as long as it takes a player whose network comes back to join again. RetroArch, on TCP, often knows at once. A player who quits normally holds nobody.

**More data.** Each packet repeats the inputs not yet confirmed, so that a lost packet doesn't make anybody wait: 5–6 kB/s per player nearby, up to 13 kB/s very far away, against 4–5 for RetroArch and 2–3 for Fightcade. Still little for any connection.

**Fewer games, fewer extras.** 256 games, all co-op, each tested; FBNeo has thousands, and Fightcade's catalog is the place for fighting games. No spectators, no replays, no rankings, no RetroAchievements. And Coinmates is young, with a small community.

## Which one to pick

- **Friends far apart, playing the co-op classics together, on a Steam Deck or anything else:** Coinmates. It's what it's made for.
- **Fighting games against people you don't know, rankings, replays:** Fightcade.
- **A game that isn't in Coinmates, players at home or with a port they can open:** RetroArch, which EmuDeck already installs. Keep in mind that some games drift apart between the players, and that it will reload the game every 10 seconds.

## How we measured

All three ran on the same computer, each player in its own copy, talking over a simulated network (Linux's `netem`: delay, jitter and lost packets, the same for all three): a home LAN, Europe (30 ms round trip), home wifi (60 ms, 2% lost), a phone (100 ms, 3% lost), Italy–Australia by cable (300 ms, 1% lost) and between two phones (600 ms, 2% lost), and outages. In all three the same bot played, pressing far more than a person does, so the corrections are a worst case for everybody. RetroArch was built from its source with hooks that only measure; Fightcade was measured through a library placed between the game and its netplay library. Each network test is a minute of play, repeated three times where it mattered; each game of the check on all the games in common, 40 seconds on a LAN with no input delay. Not tested: real routers and phone networks, and the Steam Deck itself, where Fightcade also pays for Wine or Proton.
