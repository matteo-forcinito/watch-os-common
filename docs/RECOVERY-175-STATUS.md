# Recovery AMOLED 1.75 / 1.75C - stato del collaudo

Codice in collaudo, non ancora una release verificata. Modelli separati, GPIO reset e clock audio 1.75C corretti. Recovery persistente NVS, display virtuale, BOOT tenuto cinque secondi e rilasciato, input attivato prima del display. Web automatico e sessione locale senza PIN valida solo durante la recovery, richiesta esplicita dell'utente.

1.75: build riuscita e prima immagine diagnostica 1.8.0 installata su COM14, scrittura verificata via hash. Tabelle partizioni confrontate e NVS verificata con due letture indipendenti, senza erase-flash. Gesto manuale eseguito dall'utente; diagnosi USB/avvio ancora in corso. Modifica senza PIN successiva a questa prima immagine, da ricompilare e provare.

1.75C: repository dedicato e prima compilazione in corso/completamento. MCLK16, LCD reset1, touch reset2, flash32MB, niente microSD. Identita amoled175c_watch; 1.75 standard mantiene amoled175_watch, MCLK42, reset39/40, flash16MB e microSD.

Prima di rilasciare: completare test di avvio, gesto/persistenza, sessioni automatiche e loro rifiuto fuori recovery, speaker C, compilazione di tutti i target condivisi e pacchetti di produzione senza console di sviluppo. Nessun risultato hardware ancora non verificato va dichiarato riuscito.
