; Deterministic Doom 1.9 demo used to bound the original-extender lifetime test.
; The executable remains the byte-identical shipped PCDMCORE.EXE.  Its startup
; and renderer already execute well beyond the bounded instruction/port
; thresholds before Doom consumes this native marker and exits -timedemo.
db 109                         ; Doom demo version 1.9
db 2, 1, 1                    ; skill, episode, map
db 0, 0, 0, 0                 ; deathmatch, respawn, fast, nomonsters
db 0                           ; console player
db 1, 0, 0, 0                 ; player-in-game flags
db 0, 0, 0, 0                 ; one neutral game tic
db 080h                        ; DEMOMARKER
