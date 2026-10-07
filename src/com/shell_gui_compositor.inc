; Compose bounded horizontal bands in ordinary DOS memory, then copy only
; completed pixels to VRAM. No framebuffer clear is ever presented midway
; through a normal redraw. Scratch is at most 60 KiB and may be smaller.
ui_comp_begin:
    pushad
    push es
    cmp word [ui_comp_seg],0
    jne .done
    cmp byte [ui_vbe],1
    je .packed_stride
    ; Mode 12h is indexed in the scratch band. One byte per pixel keeps the
    ; painter independent of the four VGA planes and their latches.
    movzx eax,word [ui_width]
    jmp .check_stride
.packed_stride:
    ; Compute the band before allocating: even one scanline must fit the
    ; private segment. Never accept a wrapped stride or a zero-height band.
    movzx eax,word [ui_width]
    movzx ebx,byte [vc_bytes]
    imul eax,ebx
.check_stride:
    test eax,eax
    jz .done
    cmp eax,61440
    ja .done
    mov [ui_comp_stride],ax
    mov bx,3840
.try_alloc:
    mov [ui_comp_try_paras],bx
    mov ax,bx
    shl ax,4
    cmp ax,[ui_comp_stride]
    jb .done
    ; Under Jemm (I=) the band lives in an XMS upper memory block, leaving
    ; conventional memory to the programs a DOS window runs.
    call ui_umb_alloc
    jnc .allocated
    mov bx,[ui_comp_try_paras]
    mov ah,0x48
    int 0x21
    jnc .allocated
    mov bx,[ui_comp_try_paras]
    shr bx,1
    cmp bx,256
    jae .try_alloc
    jmp .done
.allocated:
    mov [ui_comp_seg],ax
    mov ax,[ui_comp_try_paras]
    shl ax,4
    mov [ui_comp_capacity],ax
    mov bx,[ui_comp_stride]
    mov ax,[ui_comp_capacity]
    xor dx,dx
    div bx
    cmp ax,64
    jbe .height
    mov ax,64
.height:
    mov [ui_comp_rows],ax
    cmp byte [ui_vbe],1
    jne .done
    call ui_pages_begin
    call vc_session_fb_bind
    jnc .done
    cmp byte [vc_lfb],2
    jne .done                  ; ordinary banked transport remains optional
    mov byte [ui_active],0
    call ui_video_wait_release
    mov byte [ui_comp_recover],1 ; no wallpaper/pointer paint before mode retry
.done:
    pop es
    popad
    ret

; BX paragraphs -> AX segment of an XMS upper memory block; CF=1 if none.
; BX = paragraphs: an upper memory block if there is one (Jemm), else DOS
; memory. AX = segment, CF=1 when neither. The desktop's own blocks live
; there, so a DOS program in a window keeps its conventional memory.
hi_alloc:
    call ui_umb_alloc
    jnc .done
    mov ah,0x48
    int 0x21
.done:
    ret

; ES = a block from hi_alloc: back to XMS (an upper block) or to DOS.
hi_free:
    push ax
    push bx
    push dx
    mov ax,es
    cmp ax,0xA000
    jb .dos
    mov dx,ax
    mov ah,0x11
    call far [ui_xms]
    jmp .done
.dos:
    mov ah,0x49
    int 0x21
.done:
    pop dx
    pop bx
    pop ax
    ret

ui_umb_alloc:
    push bx
    push dx
    push es
    mov dx,bx
    mov ax,0x4300
    int 0x2F
    cmp al,0x80
    jne .none
    mov ax,0x4310
    int 0x2F
    mov [ui_xms],bx
    mov [ui_xms+2],es
    mov ah,0x10
    call far [ui_xms]
    cmp ax,1
    jne .none
    mov ax,bx
    clc
    jmp .done
.none:
    stc
.done:
    pop es
    pop dx
    pop bx
    ret

ui_comp_end:
    pushad
    push es
    call ui_video_wait_release
    mov ax,[ui_comp_seg]
    test ax,ax
    jz .done
    cmp ax,0xA000
    jb .dos_free
    mov dx,ax
    mov ah,0x11
    call far [ui_xms]
    jmp .freed
.dos_free:
    mov es,ax
    mov ah,0x49
    int 0x21
.freed:
    mov word [ui_comp_seg],0
    mov word [ui_comp_capacity],0
.done:
    mov byte [ui_page_enabled],0
    mov byte [ui_page_pending],0
    mov dword [ui_front_base],0
    mov eax,[vc_frame_bytes]
    mov [vc_access_bytes],eax
    mov byte [ui_composing],0
    mov byte [ui_comp_dirty],0
    pop es
    popad
    ret

ui_comp_draw:
    cmp byte [ui_active],1
    jne .inactive
    cmp byte [ui_vbe],1
    jne .surface
    cmp byte [vc_active],1
    jne .inactive
.surface:
    cmp word [ui_comp_seg],0
    je .direct
    cmp byte [ui_comp_dirty],1
    je .bounds
    mov word [ui_comp_left],0
    mov word [ui_comp_limit_top],0
    mov ax,[ui_width]
    mov [ui_comp_right],ax
    mov ax,[ui_height]
    mov [ui_comp_limit_bottom],ax
    mov word [ui_dmg],0
    mov word [ui_dmg+2],0
    mov ax,[ui_width]
    mov [ui_dmg+4],ax
    mov ax,[ui_height]
    mov [ui_dmg+6],ax
    mov byte [ui_dmg_n],1
.bounds:
    ; Damage is an exclusive rectangle. Empty/off-screen requests must not
    ; enter the band loop: a REP/LOOP count of zero can otherwise wrap.
    mov ax,[ui_comp_right]
    cmp ax,[ui_width]
    jbe .right
    mov ax,[ui_width]
    mov [ui_comp_right],ax
.right:
    cmp ax,[ui_comp_left]
    jbe .pointer
    mov ax,[ui_comp_limit_bottom]
    cmp ax,[ui_height]
    jbe .bottom_limit
    mov ax,[ui_height]
    mov [ui_comp_limit_bottom],ax
.bottom_limit:
    cmp ax,[ui_comp_limit_top]
    jbe .pointer
    cmp byte [ui_vbe],1
    je .prepare
    ; A planar write addresses a complete group of eight pixels. Expand
    ; damage to byte boundaries so no neighbouring pixel is lost.
    and word [ui_comp_left],0xFFF8
    add word [ui_comp_right],7
    and word [ui_comp_right],0xFFF8
    cmp word [ui_comp_right],640
    jbe .compose
    mov word [ui_comp_right],640
    jmp .compose
.prepare:
    call ui_comp_list
    jmp .pointer
.compose:
    call ui_comp_bands
    jmp .pointer
.direct:
    ; The VGA fallback has no off-screen page. Updating the top indicators
    ; through the full scene exposed every intermediate paint on real CRT/LCD
    ; hardware. A top-only damage rectangle can repaint its own surface.
    cmp byte [ui_comp_dirty],1
    jne .direct_scene
    cmp word [ui_comp_limit_top],0
    jne .direct_scene
    cmp word [ui_comp_limit_bottom],29
    ja .direct_scene
    cmp byte [app_overlay],0
    jne .direct_scene
    call ui_pointer_hide
    ; ui_button registers the DOS hit area while drawing it. Keep the hit
    ; table from the last full scene instead of appending once per second.
    push word [ui_hit_count]
    call ui_draw_topbar_visuals
    pop word [ui_hit_count]
    call app_desktop_topbar_paint
    jmp .pointer
.direct_scene:
    call ui_pointer_hide
    call ui_draw_scene
.pointer:
    mov byte [ui_comp_dirty],0
    call ui_pages_present
    jc .restore_pointer
    call ui_pointer_show
.done:
    ret
.restore_pointer:
    ; A failed flip disables page mode; restore a cursor hidden for syncing.
    cmp byte [ui_active],1
    jne .done
    call ui_pointer_show
    ret
.inactive:
    mov byte [ui_active],0
    mov byte [ui_comp_recover],1
    ret

; Compose the damage rectangle band by band, presenting each finished band
; to ui_present_base. Single-page presentation hides the pointer only for
; the copy of a band that overlaps it.
ui_comp_bands:
    call ui_comp_band_setup
    mov byte [ui_composing],1
    mov ax,[ui_comp_limit_top]
    mov [ui_comp_top],ax
.band:
    mov ax,[ui_comp_top]
    add ax,[ui_comp_rows]
    cmp ax,[ui_comp_limit_bottom]
    jbe .bottom
    mov ax,[ui_comp_limit_bottom]
.bottom:
    mov [ui_comp_bottom],ax
    call ui_draw_scene
    ; Leave the software cursor visible while composing. Hide it only for
    ; the brief VRAM copy of a band which actually intersects its footprint.
    mov byte [ui_comp_cursor_band],0
    cmp byte [ui_page_enabled],1
    je .present
    mov ax,[ui_mouse_y]
    cmp ax,[ui_comp_bottom]
    jae .present
    add ax,16
    cmp ax,[ui_comp_top]
    jbe .present
    mov ax,[ui_mouse_x]
    cmp ax,[ui_comp_right]
    jae .present
    add ax,24
    cmp ax,[ui_comp_left]
    jbe .present
    mov byte [ui_comp_cursor_band],1
    call ui_pointer_hide
.present:
    call ui_comp_present
    cmp byte [ui_comp_cursor_band],1
    jne .advance
    mov byte [ui_composing],0
    call ui_pointer_show
    mov byte [ui_composing],1
.advance:
    mov ax,[ui_comp_bottom]
    mov [ui_comp_top],ax
    cmp ax,[ui_comp_limit_bottom]
    jb .band
    mov byte [ui_composing],0
    ret

; Synchronize the hidden page from the visible page, then compose only this
; frame's damage. Pixel copying preserves exact occlusion and avoids invoking
; live window painters just to catch up the other page.
ui_comp_list:
    pushad
    mov byte [ui_page_pending],0
    mov eax,[ui_front_base]
    mov [ui_present_base],eax
    cmp byte [ui_page_enabled],1
    jne .current
    cmp byte [ui_pdmg_n],0
    je .current
    call ui_page_copy_needed
    ; Catch-up may have copied the front cursor. Substitute its background
    ; on the hidden page; never erase the visible source to perform a copy.
    cmp byte [ui_pointer_visible],1
    jne .current
    push fs
    mov fs,[ui_assets_seg]
    mov edi,[ui_pointer_linear]
    call ui_pointer_buffer
    sub edi,[ui_front_base]
    add edi,[vc_frame_bytes]
    sub edi,[ui_front_base]
    call vc_pointer_restore
    pop fs
.current:
    push es
    push ds
    pop es
    mov si,ui_dmg
    mov di,ui_cdmg
    mov cx,UI_DMG_MAX*4
    cld
    rep movsw
    pop es
    mov al,[ui_dmg_n]
    mov [ui_cdmg_n],al
    cmp byte [ui_page_enabled],1
    jne .compose
    push es
    push ds
    pop es
    mov di,ui_ndmg
    mov si,ui_dmg
    mov cx,UI_DMG_MAX*4
    cld
    rep movsw
    pop es
    mov al,[ui_dmg_n]
    mov [ui_ndmg_n],al
    mov eax,[vc_frame_bytes]
    sub eax,[ui_front_base]
    mov [ui_present_base],eax
    mov byte [ui_page_pending],1
.compose:
    mov si,ui_cdmg
    mov cl,[ui_cdmg_n]
    call ui_comp_rects
    popad
    ret

; The hidden page lacks previous damage, except where the new frame will
; replace it anyway. Subtract current rectangles before touching VRAM.
; With N=4 rectangles, all fragment edges lie on a (2N+1)^2 cell grid,
; so the two bounded fragment lists can represent every possible result.
ui_page_copy_needed:
    pushad
    push es
    push fs
    mov ax,[ui_assets_seg]
    mov es,ax
    mov fs,ax
    mov word [ui_catch_previous],ui_pdmg
    movzx ax,byte [ui_pdmg_n]
    mov [ui_catch_previous_left],ax
.previous:
    cmp word [ui_catch_previous_left],0
    je .done
    mov si,[ui_catch_previous]
    mov eax,[si]
    mov [fs:ui_catch_a],eax
    mov eax,[si+4]
    mov [fs:ui_catch_a+4],eax
    mov word [ui_catch_source],ui_catch_a
    mov word [ui_catch_dest],ui_catch_b
    mov word [ui_catch_count],1
    mov word [ui_catch_cover],ui_dmg
    movzx ax,byte [ui_dmg_n]
    mov [ui_catch_covers_left],ax
.cover:
    cmp word [ui_catch_covers_left],0
    je .copy
    cmp word [ui_catch_count],0
    je .next
    mov word [ui_catch_out_count],0
    mov ax,[ui_catch_source]
    mov [ui_catch_fragment],ax
    mov ax,[ui_catch_count]
    mov [ui_catch_fragments_left],ax
.fragment:
    mov si,[ui_catch_fragment]
    mov eax,[fs:si]
    mov [ui_catch_rect],eax
    mov eax,[fs:si+4]
    mov [ui_catch_rect+4],eax
    mov di,[ui_catch_cover]
    ; Intersect this fragment with the current rectangle.
    mov ax,[fs:si]
    cmp ax,[di]
    jae .left
    mov ax,[di]
.left:
    mov [ui_catch_intersection],ax
    mov ax,[fs:si+2]
    cmp ax,[di+2]
    jae .top
    mov ax,[di+2]
.top:
    mov [ui_catch_intersection+2],ax
    mov ax,[fs:si+4]
    cmp ax,[di+4]
    jbe .right
    mov ax,[di+4]
.right:
    mov [ui_catch_intersection+4],ax
    mov ax,[fs:si+6]
    cmp ax,[di+6]
    jbe .bottom
    mov ax,[di+6]
.bottom:
    mov [ui_catch_intersection+6],ax
    cmp ax,[ui_catch_intersection+2]
    jbe .whole
    mov ax,[ui_catch_intersection+4]
    cmp ax,[ui_catch_intersection]
    jbe .whole
%macro UI_PAGE_FRAGMENT 4
    mov ax,%1
    mov bx,%2
    mov cx,%3
    mov dx,%4
    call ui_page_fragment_emit
    jc .copy_full
%endmacro
    UI_PAGE_FRAGMENT [ui_catch_rect], [ui_catch_rect+2], [ui_catch_rect+4], [ui_catch_intersection+2]
    UI_PAGE_FRAGMENT [ui_catch_rect], [ui_catch_intersection+6], [ui_catch_rect+4], [ui_catch_rect+6]
    UI_PAGE_FRAGMENT [ui_catch_rect], [ui_catch_intersection+2], [ui_catch_intersection], [ui_catch_intersection+6]
    UI_PAGE_FRAGMENT [ui_catch_intersection+4], [ui_catch_intersection+2], [ui_catch_rect+4], [ui_catch_intersection+6]
    jmp .fragment_next
.whole:
    UI_PAGE_FRAGMENT [ui_catch_rect], [ui_catch_rect+2], [ui_catch_rect+4], [ui_catch_rect+6]
%unmacro UI_PAGE_FRAGMENT 4
.fragment_next:
    add word [ui_catch_fragment],8
    dec word [ui_catch_fragments_left]
    jnz .fragment
    mov ax,[ui_catch_source]
    xchg ax,[ui_catch_dest]
    mov [ui_catch_source],ax
    mov ax,[ui_catch_out_count]
    mov [ui_catch_count],ax
    add word [ui_catch_cover],8
    dec word [ui_catch_covers_left]
    jmp .cover
.copy:
    mov si,[ui_catch_source]
    mov cx,[ui_catch_count]
    call ui_page_copy_rects
    jmp .next
.copy_full:
    ; Defensive fallback preserves pixels even if a damaged count is bad.
    push fs
    push ds
    pop fs
    mov si,[ui_catch_previous]
    mov cl,1
    call ui_page_copy_rects
    pop fs
.next:
    add word [ui_catch_previous],8
    dec word [ui_catch_previous_left]
    jmp .previous
.done:
    pop fs
    pop es
    popad
    ret

ui_page_fragment_emit:
    pushad
    cmp cx,ax
    jbe .empty
    cmp dx,bx
    jbe .empty
    mov si,[ui_catch_out_count]
    cmp si,UI_CATCH_MAX
    jae .full
    shl si,3
    mov di,[ui_catch_dest]
    add di,si
    mov [es:di],ax
    mov [es:di+2],bx
    mov [es:di+4],cx
    mov [es:di+6],dx
    inc word [ui_catch_out_count]
.empty:
    clc
    popad
    ret
.full:
    stc
    popad
    ret

; FS:SI = rectangle list (left,top,right,bottom), CL = count. Copy each
; completed rectangle from the visible page into the hidden page. Prefer
; CVSESSION's physical-LFB copy; otherwise use the compositor scratch band
; and normal VBE get/put paths, which also cover banked modes.
ui_page_copy_rects:
    pushad
    push es
    push fs
    movzx eax,cl
    mov [ui_copy_rect_count],ax
    mov [ui_copy_rect_ptr],si
    mov ax,fs
    mov [ui_copy_rect_seg],ax
    mov eax,[ui_front_base]
    mov [ui_copy_source_base],eax
    mov eax,[vc_frame_bytes]
    sub eax,[ui_front_base]
    mov [ui_copy_dest_base],eax
.rect:
    cmp word [ui_copy_rect_count],0
    je .done
    mov si,[ui_copy_rect_ptr]
    mov fs,[ui_copy_rect_seg]
    mov ax,[fs:si]
    mov [ui_copy_left],ax
    mov ax,[fs:si+2]
    mov [ui_copy_top],ax
    mov ax,[fs:si+4]
    mov [ui_copy_right],ax
    mov ax,[fs:si+6]
    mov [ui_copy_bottom],ax
    mov ax,[ui_copy_right]
    cmp ax,[ui_copy_left]
    jbe .next
    mov ax,[ui_copy_bottom]
    cmp ax,[ui_copy_top]
    jbe .next
    movzx eax,word [ui_copy_right]
    movzx edx,word [ui_copy_left]
    sub eax,edx
    movzx edx,byte [vc_bytes]
    imul eax,edx
    mov [ui_copy_row_bytes],ax
    mov esi,[ui_copy_source_base]
    movzx eax,word [ui_copy_top]
    movzx edx,word [vc_pitch]
    imul eax,edx
    add esi,eax
    movzx eax,word [ui_copy_left]
    movzx edx,byte [vc_bytes]
    imul eax,edx
    add esi,eax
    mov edi,[ui_copy_dest_base]
    movzx eax,word [ui_copy_top]
    movzx edx,word [vc_pitch]
    imul eax,edx
    add edi,eax
    movzx eax,word [ui_copy_left]
    movzx edx,byte [vc_bytes]
    imul eax,edx
    add edi,eax
    mov cx,[ui_copy_row_bytes]
    mov dx,[ui_copy_bottom]
    sub dx,[ui_copy_top]
    call vc_fb_copy_rows
    jnc .next
    ; Scratch fallback works for a bound-service miss and for banked VBE.
    movzx ebx,word [ui_copy_row_bytes]
    test ebx,ebx
    jz .next
    movzx eax,word [ui_comp_capacity]
    xor edx,edx
    div ebx
    test eax,eax
    jz .next
    cmp eax,64
    jbe .rows_ready
    mov eax,64
.rows_ready:
    mov [ui_copy_rows],ax
    mov ax,[ui_copy_top]
    mov [ui_copy_y],ax
.band:
    mov ax,[ui_copy_bottom]
    sub ax,[ui_copy_y]
    cmp ax,[ui_copy_rows]
    jbe .band_rows
    mov ax,[ui_copy_rows]
.band_rows:
    test ax,ax
    jz .next
    mov [ui_copy_chunk_rows],ax
    mov edi,[ui_copy_source_base]
    movzx eax,word [ui_copy_y]
    movzx edx,word [vc_pitch]
    imul eax,edx
    add edi,eax
    movzx eax,word [ui_copy_left]
    movzx edx,byte [vc_bytes]
    imul eax,edx
    add edi,eax
    mov cx,[ui_copy_row_bytes]
    mov dx,[ui_copy_chunk_rows]
    mov bx,cx
    xor si,si
    mov ax,[ui_comp_seg]
    mov fs,ax
    call vc_fb_get_rows
    mov edi,[ui_copy_dest_base]
    movzx eax,word [ui_copy_y]
    movzx edx,word [vc_pitch]
    imul eax,edx
    add edi,eax
    movzx eax,word [ui_copy_left]
    movzx edx,byte [vc_bytes]
    imul eax,edx
    add edi,eax
    mov cx,[ui_copy_row_bytes]
    mov dx,[ui_copy_chunk_rows]
    mov bx,cx
    xor si,si
    call vc_fb_put_rows
    mov ax,[ui_copy_y]
    add ax,[ui_copy_chunk_rows]
    mov [ui_copy_y],ax
    jmp .band
.next:
    add word [ui_copy_rect_ptr],8
    dec word [ui_copy_rect_count]
    jmp .rect
.done:
    pop fs
    pop es
    popad
    ret

; SI = rectangle list (left, top, right, bottom words), CL = count: compose
; each into ui_present_base.
ui_comp_rects:
    pushad
    movzx cx,cl
.rect:
    jcxz .done
    mov ax,[si]
    mov [ui_comp_left],ax
    mov ax,[si+2]
    mov [ui_comp_limit_top],ax
    mov ax,[si+4]
    mov [ui_comp_right],ax
    mov ax,[si+6]
    mov [ui_comp_limit_bottom],ax
    mov ax,[ui_comp_right]
    cmp ax,[ui_comp_left]
    jbe .skip
    mov ax,[ui_comp_limit_bottom]
    cmp ax,[ui_comp_limit_top]
    jbe .skip
    pusha
    call ui_comp_bands
    popa
.skip:
    add si,8
    dec cx
    jmp .rect
.done:
    popad
    ret

; [ui_ra_l..ui_ra_b] = rectangle (exclusive right/bottom). SI = list, BX = its
; count byte, AL = capacity. The rectangle merges into an entry it overlaps
; or nearly touches; else it is appended; a full list merges it into the
; entry whose area grows least.
ui_dmg_insert:
    pushad
    mov [ui_ra_count],bx
    mov [ui_ra_cap],al
    movzx cx,byte [bx]
    mov di,si
    xor bx,bx
.find:
    cmp bx,cx
    jae .append
    mov ax,[ui_ra_r]
    add ax,UI_DMG_SLACK
    cmp [di],ax
    ja .next
    mov ax,[di+4]
    add ax,UI_DMG_SLACK
    cmp ax,[ui_ra_l]
    jb .next
    mov ax,[ui_ra_b]
    add ax,UI_DMG_SLACK
    cmp [di+2],ax
    ja .next
    mov ax,[di+6]
    add ax,UI_DMG_SLACK
    cmp ax,[ui_ra_t]
    jb .next
    jmp .merge
.next:
    add di,8
    inc bx
    jmp .find
.append:
    cmp cl,[ui_ra_cap]
    jae .least
    mov ax,[ui_ra_l]
    mov [di],ax
    mov ax,[ui_ra_t]
    mov [di+2],ax
    mov ax,[ui_ra_r]
    mov [di+4],ax
    mov ax,[ui_ra_b]
    mov [di+6],ax
    mov bx,[ui_ra_count]
    inc byte [bx]
    jmp .done
.least:
    mov dword [ui_ra_best],0xFFFFFFFF
    mov [ui_ra_best_at],si
    mov di,si
    xor bx,bx
.scan:
    cmp bx,cx
    jae .take
    call ui_dmg_growth
    cmp eax,[ui_ra_best]
    jae .scan_next
    mov [ui_ra_best],eax
    mov [ui_ra_best_at],di
.scan_next:
    add di,8
    inc bx
    jmp .scan
.take:
    mov di,[ui_ra_best_at]
.merge:
    mov ax,[ui_ra_l]
    cmp ax,[di]
    jae .merge_top
    mov [di],ax
.merge_top:
    mov ax,[ui_ra_t]
    cmp ax,[di+2]
    jae .merge_right
    mov [di+2],ax
.merge_right:
    mov ax,[ui_ra_r]
    cmp ax,[di+4]
    jbe .merge_bottom
    mov [di+4],ax
.merge_bottom:
    mov ax,[ui_ra_b]
    cmp ax,[di+6]
    jbe .done
    mov [di+6],ax
.done:
    popad
    ret

; DI = list entry: EAX = area of (entry + ui_ra) minus area of the entry.
ui_dmg_growth:
    push ebx
    push ecx
    push edx
    mov ax,[di]
    cmp ax,[ui_ra_l]
    jbe .left
    mov ax,[ui_ra_l]
.left:
    mov bx,[di+4]
    cmp bx,[ui_ra_r]
    jae .right
    mov bx,[ui_ra_r]
.right:
    sub bx,ax
    mov ax,[di+2]
    cmp ax,[ui_ra_t]
    jbe .top
    mov ax,[ui_ra_t]
.top:
    mov cx,[di+6]
    cmp cx,[ui_ra_b]
    jae .bottom
    mov cx,[ui_ra_b]
.bottom:
    sub cx,ax
    movzx eax,bx
    movzx ecx,cx
    imul eax,ecx
    mov ebx,eax
    movzx eax,word [di+4]
    movzx ecx,word [di]
    sub eax,ecx
    movzx ecx,word [di+6]
    movzx edx,word [di+2]
    sub ecx,edx
    imul eax,ecx
    sub ebx,eax
    mov eax,ebx
    pop edx
    pop ecx
    pop ebx
    ret

; Hardware pages are optional. Some firmware omits static image-page counts;
; require two frames in the active scanline/VRAM allocation, then prove that
; set/get-display-start can select page one and restore page zero.
; No retrace polling or blocking "wait for retrace" firmware request is used.
ui_pages_begin:
    pushad
    mov byte [ui_page_enabled],0
    mov byte [ui_page_pending],0
    mov dword [ui_front_base],0
    cmp byte [ui_page_failed],0
    jne .done
    movzx eax,word [ui_height]
    shl eax,1
    movzx edx,word [vc_scan_lines]
    cmp eax,edx
    ja .done
    mov eax,[vc_frame_bytes]
    shl eax,1
    movzx edx,word [vc_controller+18]
    shl edx,16
    cmp eax,edx
    ja .done
    cmp byte [vc_lfb],0
    jne .probe
    dec eax
    shr eax,16
    movzx edx,word [vc_bank_step]
    imul eax,edx
    cmp eax,65535
    ja .done
.probe:
    mov dx,[ui_height]
    call ui_pages_switch
    jc .unsupported
    xor dx,dx
    call ui_pages_switch
    jc .unsupported
    mov eax,[vc_frame_bytes]
    shl eax,1
    mov [vc_access_bytes],eax
    mov byte [ui_page_enabled],1
    mov word [ui_pdmg],0
    mov word [ui_pdmg+2],0
    mov ax,[ui_width]
    mov [ui_pdmg+4],ax
    mov ax,[ui_height]
    mov [ui_pdmg+6],ax
    mov byte [ui_pdmg_n],1
    jmp .done
.unsupported:
    mov byte [ui_page_failed],1
    mov byte [ui_page_enabled],0
    mov byte [ui_page_pending],0
    mov dword [ui_front_base],0
    mov dword [ui_present_base],0
    mov eax,[vc_frame_bytes]
    mov [vc_access_bytes],eax
    xor bx,bx
    xor cx,cx
    xor dx,dx
    mov ax,0x4F07
    call vc_bios
.done:
    popad
    ret

; DX = first visible scanline. Check the readback rather than trusting AX
; alone; a firmware may accept a request but retain the previous viewport.
ui_pages_switch:
    pushad
    call vc_session_fb_present
    jnc .ready
    cmp byte [vc_session_gpu],0
    jne .fail                   ; an owned GPU error is not a VBE page flip
    mov [ui_page_scanline],dx
    xor bx,bx
    xor cx,cx
    mov ax,0x4F07
    call vc_bios
    cmp ax,0x004F
    jne .fail
    mov bx,1
    mov ax,0x4F07
    call vc_bios
    cmp ax,0x004F
    jne .fail                  ; BL=1 not supported by firmware: cannot verify flip
    cmp word [vc_bios_cx],0
    jne .fail
    mov ax,[ui_page_scanline]
    cmp ax,[vc_bios_dx]
    je .ready
    ; Firmware may latch CRTC start address at next vertical retrace.
    ; Give it a brief spin for retrace before treating as failure.
    mov cx,4000
.wait_retrace:
    push cx
    mov bx,1
    mov ax,0x4F07
    call vc_bios
    pop cx
    cmp ax,0x004F
    jne .fail
    mov ax,[ui_page_scanline]
    cmp ax,[vc_bios_dx]
    je .ready
    loop .wait_retrace
    jmp .fail
.ready:
    popad
    clc
    ret
.fail:
    popad
    stc
    ret

; Service native scanout after a complete pointer update, including idle
; desktop iterations. Hidden-page writes alone never trigger a GPU upload.
ui_gpu_poll:
    cmp byte [vc_session_gpu],1 ; physical scanout changes only at page flips
    jne .done
    pushad
    xor dx,dx
    cmp dword [ui_front_base],0
    je .present
    mov dx,[ui_height]
.present:
    call vc_session_fb_present
    popad
.done:
    ret

ui_pages_present:
    cmp byte [ui_vbe],1
    jne .ready
    cmp byte [vc_active],1
    jne .inactive
    cmp byte [ui_page_pending],1
    jne .ready
    pushad
    mov byte [ui_page_pending],0
    ; Both pages retain their own save-under. Prepare the new cursor while
    ; hidden; only erase the old one after that page is no longer visible.
    push dword [ui_pointer_linear]
    push word [ui_pointer_visible]
    mov eax,[ui_present_base]
    mov [ui_front_base],eax
    mov byte [ui_pointer_visible],0
    call ui_pointer_show
    xor dx,dx
    cmp dword [ui_present_base],0
    je .flip
    mov dx,[ui_height]
.flip:
    call ui_pages_switch
    pop ax
    pop edi
    jc .fail
    test al,al
    jz .damage
    push fs
    mov fs,[ui_assets_seg]
    call ui_pointer_buffer
    call vc_pointer_restore
    pop fs
.damage:
    push es
    push ds
    pop es
    mov si,ui_ndmg
    mov di,ui_pdmg
    mov cx,UI_DMG_MAX*4
    cld
    rep movsw
    pop es
    mov al,[ui_ndmg_n]
    mov [ui_pdmg_n],al
    popad
.ready:
    clc
    ret
.fail:
    mov byte [ui_pointer_visible],0
    call ui_pages_failure
    popad
    stc
    ret
.inactive:
    pushad
    call ui_pages_failure
    popad
    stc
    ret

ui_pages_failure:
    cmp byte [vc_lfb],2
    jne .ordinary
    mov byte [ui_active],0
    mov byte [ui_comp_recover],1
    ret                        ; retained page/mapping state belongs to cleanup
.ordinary:
    mov byte [ui_page_failed],1
    mov byte [ui_page_enabled],0
    mov byte [ui_page_pending],0
    mov byte [ui_comp_recover],0
    mov dword [ui_front_base],0
    mov dword [ui_present_base],0
    mov eax,[vc_frame_bytes]
    mov [vc_access_bytes],eax
    mov byte [ui_full_redraw],1
    xor bx,bx
    xor cx,cx
    xor dx,dx
    mov ax,0x4F07
    call vc_bios
    ret

; Pack only the damaged horizontal span into the scratch segment. A narrow
; window on a 2K desktop should not replay the scene once per six scanlines
; merely because the physical screen is wide. Coordinates remain absolute
; at the public draw API; only scratch addresses subtract ui_comp_left.
ui_comp_band_setup:
    pushad
    mov ax,[ui_comp_right]
    sub ax,[ui_comp_left]
    cmp byte [ui_vbe],1
    jne .stride
    movzx bx,byte [vc_bytes]
    mul bx
.stride:
    mov [ui_comp_stride],ax
    mov bx,ax
    mov ax,[ui_comp_capacity]
    xor dx,dx
    div bx
    cmp ax,64
    jbe .rows
    mov ax,64
.rows:
    mov [ui_comp_rows],ax
    popad
    ret

; ui_rect has already checked the physical screen bounds.
ui_comp_rect:
    mov [ui_fill_color],al
    mov ax,bx
    add ax,cx
    cmp bx,[ui_comp_right]
    jae .done
    cmp ax,[ui_comp_left]
    jbe .done
    cmp bx,[ui_comp_left]
    jae .left
    mov bx,[ui_comp_left]
.left:
    cmp ax,[ui_comp_right]
    jbe .right
    mov ax,[ui_comp_right]
.right:
    sub ax,bx
    mov cx,ax
    mov ax,dx
    add ax,si
    cmp dx,[ui_comp_bottom]
    jae .done
    cmp ax,[ui_comp_top]
    jbe .done
    cmp dx,[ui_comp_top]
    jae .top
    mov dx,[ui_comp_top]
.top:
    cmp ax,[ui_comp_bottom]
    jbe .bottom
    mov ax,[ui_comp_bottom]
.bottom:
    sub ax,dx
    mov bp,ax
    mov [ui_fill_width],cx
    mov ax,dx
    sub ax,[ui_comp_top]
    mul word [ui_comp_stride]
    mov di,ax
    mov ax,bx
    sub ax,[ui_comp_left]
    cmp byte [ui_vbe],1
    jne .indexed_offset
    movzx cx,byte [vc_bytes]
    mul cx
.indexed_offset:
    add di,ax
    mov ax,[ui_comp_seg]
    mov es,ax
    cmp byte [ui_vbe],1
    jne .indexed
    movzx bx,byte [ui_fill_color]
    shl bx,2
    mov eax,[vc_colors+bx]
    jmp .row
.indexed:
    mov al,[ui_fill_color]
.row:
    push di
    mov cx,[ui_fill_width]
    cmp byte [ui_vbe],1
    jne .bytes
    cmp byte [vc_bytes],4
    je .dwords
    cmp byte [vc_bytes],2
    je .words
    cmp byte [vc_bytes],3
    je .triples
    jmp .bytes
.bytes:
    rep stosb
    jmp .next
.words:
    rep stosw
    jmp .next
.dwords:
    rep stosd
    jmp .next
.triples:
    mov ebx,eax
.triple:
    stosw
    shr eax,16
    stosb
    mov eax,ebx
    loop .triple
.next:
    pop di
    add di,[ui_comp_stride]
    dec bp
    jnz .row
.done:
    pop es
    popad
    ret

ui_comp_glyph:
    pushad
    push es
    ; Most scene glyphs do not intersect a short scanline band. Reject the
    ; complete footprint before visiting sixteen clipped rows per character.
    cmp dx,[ui_comp_bottom]
    jae .done
    mov ax,dx
    add ax,16
    cmp ax,[ui_comp_top]
    jbe .done
    cmp bx,[ui_comp_right]
    jae .done
    mov ax,bx
    add ax,16
    cmp ax,[ui_comp_left]
    jbe .done
    mov bp,16
    mov ax,[ui_comp_seg]
    mov es,ax
.row:
    cmp dx,[ui_comp_top]
    jb .skip
    cmp dx,[ui_comp_bottom]
    jae .done
    push dx
    mov ax,dx
    sub ax,[ui_comp_top]
    mul word [ui_comp_stride]
    mov di,ax
    mov ax,bx
    sub ax,[ui_comp_left]
    cmp byte [ui_vbe],1
    jne .indexed_offset
    movzx cx,byte [vc_bytes]
    mul cx
.indexed_offset:
    add di,ax
    pop dx
    mov [ui_comp_glyph_x],bx
    push bx
    mov bx,[fs:si]
    mov cx,16
.pixel:
    shl bx,1
    jnc .next
    mov ax,[ui_comp_glyph_x]
    cmp ax,[ui_comp_left]
    jb .next
    cmp ax,[ui_comp_right]
    jae .next
    push bx
    cmp byte [ui_vbe],1
    jne .indexed_pixel
    movzx bx,byte [ug_text_color]
    shl bx,2
    mov eax,[vc_colors+bx]
    pop bx
    cmp byte [vc_bytes],4
    je .dword
    cmp byte [vc_bytes],1
    je .byte
    mov [es:di],ax
    cmp byte [vc_bytes],2
    je .next
    shr eax,16
    mov [es:di+2],al
    jmp .next
.dword:
    mov [es:di],eax
    jmp .next
.byte:
    mov [es:di],al
    jmp .next
.indexed_pixel:
    mov al,[ug_text_color]
    mov [es:di],al
    pop bx
.next:
    inc word [ui_comp_glyph_x]
    cmp byte [ui_vbe],1
    jne .indexed_advance
    movzx ax,byte [vc_bytes]
    add di,ax
    jmp .advance_done
.indexed_advance:
    inc di
.advance_done:
    loop .pixel
    pop bx
.skip:
    add si,2
    inc dx
    dec bp
    jnz .row
.done:
    pop es
    popad
    ret

ui_comp_present:
    cmp byte [ui_vbe],1
    je .packed
    call ui_vga_present
    ret
.packed:
    ; Write each completed band row once, sequentially, without reading VRAM.
    pushad
    push fs
    mov ax,[ui_comp_right]
    cmp ax,[ui_comp_left]
    jbe .done
    mov ax,[ui_comp_bottom]
    cmp ax,[ui_comp_top]
    jbe .done
    mov fs,[ui_comp_seg]
    movzx ebx,byte [vc_bytes]
    movzx eax,word [ui_comp_left]
    imul eax,ebx
    movzx edi,word [ui_comp_top]
    movzx edx,word [vc_pitch]
    imul edi,edx
    add edi,eax
    add edi,[ui_present_base]
    mov cx,[ui_comp_right]
    sub cx,[ui_comp_left]
    imul cx,bx
    mov dx,[ui_comp_bottom]
    sub dx,[ui_comp_top]
    xor si,si
    mov bx,[ui_comp_stride]
    call vc_fb_put_rows
.done:
    pop fs
    popad
    ret

%include "src/com/vga_band_present.inc"
VGA_INDEXED_PRESENT ui_vga_present, ui_comp_seg, ui_comp_left, ui_comp_right, ui_comp_top, ui_comp_bottom, ui_vga_row_bytes, ui_vga_rows, ui_vga_origin, ui_vga_plane_bit

; Extend the invalidated region with a physical rectangle. The previous and
; next bounds of a moving window are both included, including its shadow.
ui_comp_damage:
    pushad
    test cx,cx
    jz .done
    test si,si
    jz .done
    cmp bx,[ui_width]
    jae .done
    cmp dx,[ui_height]
    jae .done
    ; Clamp widths before addition so even a large extent cannot wrap.
    mov ax,[ui_width]
    sub ax,bx
    cmp cx,ax
    jbe .width
    mov cx,ax
.width:
    mov ax,[ui_height]
    sub ax,dx
    cmp si,ax
    jbe .height
    mov si,ax
.height:
    add cx,bx
    add si,dx
    cmp cx,[ui_width]
    jbe .right
    mov cx,[ui_width]
.right:
    cmp si,[ui_height]
    jbe .bottom
    mov si,[ui_height]
.bottom:
    mov [ui_ra_l],bx
    mov [ui_ra_t],dx
    mov [ui_ra_r],cx
    mov [ui_ra_b],si
    cmp byte [ui_comp_dirty],1
    je .listed
    mov byte [ui_dmg_n],0
.listed:
    pusha
    mov si,ui_dmg
    mov bx,ui_dmg_n
    mov al,UI_DMG_MAX
    call ui_dmg_insert
    popa
    cmp byte [ui_comp_dirty],1
    je .extend
    mov [ui_comp_left],bx
    mov [ui_comp_right],cx
    mov [ui_comp_limit_top],dx
    mov [ui_comp_limit_bottom],si
    mov byte [ui_comp_dirty],1
    jmp .done
.extend:
    cmp bx,[ui_comp_left]
    jae .top
    mov [ui_comp_left],bx
.top:
    cmp dx,[ui_comp_limit_top]
    jae .wide
    mov [ui_comp_limit_top],dx
.wide:
    cmp cx,[ui_comp_right]
    jbe .tall
    mov [ui_comp_right],cx
.tall:
    cmp si,[ui_comp_limit_bottom]
    jbe .done
    mov [ui_comp_limit_bottom],si
.done:
    popad
    ret

ui_comp_damage_drag:
    pushad
    cmp byte [ui_dragging],3
    je .native
    cmp byte [ui_dragging],4
    je .native
    mov bx,[ui_wx]
    mov dx,[ui_wy]
    mov cx,[ui_ww]
    add cx,5
    mov si,[ui_wh]
    add si,6
    jmp .damage
.native:
    movzx bx,byte [ui_window_drag]
    call ui_window_bounds
    add cx,5
    add si,6
.damage:
    call ui_comp_damage
    popad
    ret

UI_DMG_MAX equ 4                ; separate damage rectangles per frame
UI_CATCH_MAX equ (UI_DMG_MAX*2+1)*(UI_DMG_MAX*2+1)
UI_CDMG_MAX equ 8               ; current plus the back page's previous ones
UI_DMG_SLACK equ 16             ; nearer rectangles merge (fewer scene passes)
ui_dmg_n db 0
ui_ndmg_n db 0
ui_pdmg_n db 0
ui_cdmg_n db 0
ui_ra_cap db 0
    align 2
ui_dmg times UI_DMG_MAX*4 dw 0  ; this frame's damage
ui_ndmg times UI_DMG_MAX*4 dw 0 ; becomes the previous damage at the flip
ui_pdmg times UI_DMG_MAX*4 dw 0 ; what the back page lacks
ui_cdmg times UI_CDMG_MAX*4 dw 0 ; composed now
ui_ra_l dw 0
ui_ra_t dw 0
ui_ra_r dw 0
ui_ra_b dw 0
ui_ra_count dw 0
ui_ra_best_at dw 0
ui_ra_best dd 0
ui_comp_seg dw 0
ui_comp_capacity dw 0
ui_comp_try_paras dw 0
ui_comp_stride dw 0
ui_comp_rows dw 0
ui_comp_top dw 0
ui_comp_bottom dw 0
ui_composing db 0
ui_scene_covered db 0           ; this band lies in a window's client area
ui_comp_dirty db 0
ui_comp_left dw 0
ui_comp_right dw 0
ui_comp_limit_top dw 0
ui_comp_limit_bottom dw 0
ui_comp_x_bytes dw 0
ui_comp_row_bytes dw 0
ui_comp_glyph_x dw 0
ui_comp_cursor_band db 0
ui_vga_row_bytes dw 0
ui_vga_rows dw 0
ui_vga_origin dw 0
ui_vga_plane_bit db 0
ui_copy_rect_count dw 0
ui_copy_rect_ptr dw 0
ui_copy_rect_seg dw 0
ui_copy_source_base dd 0
ui_copy_dest_base dd 0
ui_copy_left dw 0
ui_copy_top dw 0
ui_copy_right dw 0
ui_copy_bottom dw 0
ui_copy_row_bytes dw 0
ui_copy_rows dw 0
ui_copy_chunk_rows dw 0
ui_copy_y dw 0
ui_catch_previous dw 0
ui_catch_previous_left dw 0
ui_catch_source dw 0
ui_catch_dest dw 0
ui_catch_count dw 0
ui_catch_cover dw 0
ui_catch_covers_left dw 0
ui_catch_fragment dw 0
ui_catch_fragments_left dw 0
ui_catch_out_count dw 0
ui_catch_rect times 4 dw 0
ui_catch_intersection times 4 dw 0
ui_page_enabled db 0
ui_page_pending db 0
ui_page_failed db 0
ui_comp_recover db 0
ui_front_base dd 0
ui_xms dd 0
ui_present_base dd 0
ui_page_scanline dw 0
