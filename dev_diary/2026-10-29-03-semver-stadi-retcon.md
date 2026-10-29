# Versioni secondo Semantic Versioning 2.0.0, stadi e riallineamento delle etichette

- **Data:** 2026-10-29 09:50
- **Tipo:** decisione
- **Sostituisce:** [2026-10-29-02](2026-10-29-02-versione-beta-1-0-1.md) (1.0.1 beta)

## Fatti

1. **Riferimenti:**
   - ISO/IEC/IEEE 12207 e ISO 10007 chiedono un'identità univoca e tracciabile per ogni rilascio e baseline, senza prescrivere un formato;
   - ISO/IEC 19770-2 (SWID) include `semver` tra gli schemi di versione;
   - Semantic Versioning 2.0.0 (https://semver.org/spec/v2.0.0.html):
     - §4: 0.y.z è lo sviluppo iniziale;
     - §5: la 1.0.0 definisce l'API pubblica;
     - §9: le pre-release sono identificatori dopo `-` della versione che precedono;
     - §10: i metadati di build vanno dopo `+`;
     - §11: la precedenza è `1.0.0-alpha.1 < 1.0.0-beta.1 < 1.0.0-rc.1 < 1.0.0`.
2. **Problemi dello schema precedente:**
   - "1.0.1 alpha" presuppone una 1.0.0 stabile mai esistita;
   - i tag `v<versione>-stable.<n>` sono pre-release per SemVer;
   - l'etichetta "pre-alpha" si ordina dopo "alpha";
   - i tag 0.8.3 mettono build e commit nella parte di pre-release.

## Decisione del proprietario (2026-10-29)

- **Schema.** Le versioni seguono SemVer 2.0.0:

  | Stadio | Versione | Tag | Prerelease GitHub |
  |---|---|---|---|
  | Sviluppo | `0.y.z` | nessuno o `v0.y.z` | sì |
  | Alpha | `1.0.0-alpha.N` | `v1.0.0-alpha.N` | sì |
  | Beta | `1.0.0-beta.N` | `v1.0.0-beta.N` | sì |
  | Release candidate | `1.0.0-rc.N` | `v1.0.0-rc.N` | sì |
  | Stabile | `1.0.0`, poi `1.0.1`, `1.1.0`, `2.0.0` | `v1.0.0` | no |

- **Build e nome.** I metadati di build sono `+build.<id>.g<commit>`. Il nome leggibile è, per esempio, "CiukiOS 1.0.0 Beta 1".
- **API pubblica.** È l'API nativa documentata in `docs/developer-api.md` più i formati su disco; una rottura richiede una nuova MAJOR.
- **Release candidate.** Lo stadio rc si aggiunge tra beta e 1.0.0 stabile.
- **Riallineamento.** Le etichette mai pubblicate cambiano; i tag già pubblicati restano invariati, come prescrive la sezione "On Re-tagging" del manuale di `git tag`.

  | Etichetta precedente | Pubblicata | Nuova identità |
  |---|---|---|
  | `v0.8.3-…-b847`, `v0.8.3-…-b849` (ramo `legacy-0.8`) | sì | invariata; corrisponde a `0.8.3+b847…`, `0.8.3+b849.g868cac9e` |
  | 0.9.0 pre-alpha | no | `0.9.0` |
  | 0.9.2 pre-alpha | no | `0.9.2` |
  | 1.0.0 alpha (immagini hardware del 21–22 ottobre) | no | `1.0.0-alpha.1`, immagini distinte da `+build` |
  | 1.0.1 alpha (25 ottobre) | no | `1.0.0-alpha.2` |
  | 1.0.1 beta (29 ottobre) | no | `1.0.0-beta.1` |

  Le voci precedenti del diario mantengono le etichette dell'epoca; questa tabella le collega alle nuove identità.
- **Esecuzione.** È la direttiva fx-semver: `config/version.json` con la stringa SemVer completa, validazione, build, loader, About, `uname`, `release_stable.py` con tag `v<versione>` e flag di prerelease dalla versione, CHANGELOG con la sezione "Versioning". Entra in main prima della build finale della Beta.
