# Pulizia di cartella locale e repository, nuova struttura dei test

- **Data:** 2026-10-09
- **Tipo:** modifica
- **Versione/commit:** 0.8.3 (stesso commit della pulizia)
- **Stato:** completato (GitHub: push 868cac9, prerelease b849, tenute b847 e b849, solo `main`)

## Contesto

Pulizia di documenti, script vecchi, build e test non più usati, e una
struttura di test che non crei un build diverso per ogni prova, per usare
meglio le risorse della workstation (14 GiB di RAM, `/tmp` in RAM).

## Cosa cambia

- **Spazio:** la cartella di lavoro passa da 121 GB a circa 3,4 GB.
  `build/` mantiene solo `external/`, `downloads/`, `tools/`, il pacchetto
  Windows corrente e l'output del build canonico.
- **Documenti:** `docs/` contiene solo la guida al build, la guida al pacchetto
  Windows, gli screenshot 0.8.3 e il nuovo design dei test. Tutto il resto
  (circa 540 file, più le note di `setup/` e gli handoff pubblicati) è in
  `legacy/CiukiOS-docs-legacy-2026-10-09.zip`.
- **Script:** da 432 a 66 file. Restano solo quelli usati dal build
  (verificati con un build completo da zero sotto `strace`), dal CD,
  dall'avvio, dal rilascio, dai download per un clone nuovo e dalla raccolta
  dei log dai dischi fisici. Gli altri circa 370 (gate QEMU, test host,
  sonde, diagnosi una tantum, floppy, macOS) e il vecchio Makefile sono in
  `legacy/CiukiOS-scripts-legacy-2026-10-09.zip`.
- **Makefile:** ridotto ai target validi; `build-full` e `build-full-cd`
  girano già nello scope con limiti di memoria.
- **Materiale privato** (log dei PC fisici, vecchi handoff, backup) spostato
  in `legacy/local/`, non pubblicato.
- Eliminati i file temporanei di CiukiOS in `/tmp` e nella radice del repo.
- **Nuova struttura dei test**
  ([`docs/design/test-architecture.md`](../docs/design/test-architecture.md)):
  un'immagine canonica per stato del sorgente; test scelti all'avvio tramite
  l'elemento QEMU fw_cfg `opt/it.alcybercloud.ciukios/test` invece dei flag di
  build; overlay qcow2 copy-on-write su disco invece di copie da 128 MB;
  un solo runner con suite dichiarate; un QEMU alla volta con limite di
  memoria; conservazione solo dei fallimenti e degli ultimi 5 run. Da
  implementare come parte della fase F0 delle nuove fondamenta.
- **Direttive per tutti gli agenti** aggiornate in `AGENTS.md`: archivio
  legacy, pulizia del repository e uso delle risorse, regole dei test.

## Verifica

- `make build-full` completo da zero dopo la pulizia: riuscito, pacchetto
  Windows rigenerato (SHA-256 `fc3ea6da…fd155`).
- `make qemu-test-full` (QEMU/KVM, limite 1,5 GB): PASS, passaggio al kernel e
  desktop pronto; nessun processo QEMU rimasto.
- Integrità degli zip verificata con `unzip -t` e `zstd -t`.
- Il build del CD (`make build-full-cd`) non è stato rieseguito.

## Riferimenti

- [`legacy/README.md`](../legacy/README.md)
- [`docs/design/test-architecture.md`](../docs/design/test-architecture.md)
