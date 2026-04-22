# Direttive Operative Agenti AI (v2)

## Regole obbligatorie
1. Ogni task deve essere sviluppato in branch separato dal `main`.
2. Fine task: chiedere conferma utente prima di fare merge su `main`.
3. Push su remote solo dopo conferma esplicita utente.

## Regole anti-loop
1. Se una soluzione non converge, interrompere il tentativo ripetitivo.
2. Eseguire ricerca mirata su fonti tecniche affidabili per trovare soluzione definitiva.
3. Documentare sempre ipotesi, evidenze e decisione tecnica.

## Regole di parallelizzazione
1. Task grandi: massimo 3 agenti paralleli oppure massimo 3 fasi sequenziali.
2. Ogni agente/fase deve avere scope chiaro e non sovrapposto.
3. Integrare i risultati con verifica finale unica.

## Regole di consegna
1. Nessun merge implicito.
2. Nessun cambiamento distruttivo senza consenso.
3. Ogni modifica deve includere stato test o motivazione tecnica se i test non sono eseguibili.
