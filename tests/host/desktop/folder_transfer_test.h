/* CiukiOS has no per-thread CPU clock; host latency coverage uses it. */
#ifdef CLOCK_THREAD_CPUTIME_ID
#define FOLDER_CPU_CLOCK CLOCK_THREAD_CPUTIME_ID
#else
#define FOLDER_CPU_CLOCK CLOCK_MONOTONIC
#endif
/* SPDX-License-Identifier: MIT
 * Block production transfer I/O while replaying real shell input. */
static uint64_t folder_clock(clockid_t clock)
{ struct timespec t; assert(!clock_gettime(clock,&t)); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void folder_transfer_cases(struct fm_model *other)
{
    fake_file("/Documents/Worker.mp3",30000,7); assert(!fm_refresh(other));
    assert(fm_select_name(other,"Worker.mp3",0)>=0 && !fm_clipboard(other,0) && !fm_clipboard_save(other));
    pthread_mutex_lock(&poll_lock); copy_hold=1; copy_entered=copy_release=0; pthread_mutex_unlock(&poll_lock);
    command(DF_PASTE,-1);
    pthread_mutex_lock(&poll_lock); while(!copy_entered) pthread_cond_wait(&poll_changed,&poll_lock); pthread_mutex_unlock(&poll_lock);
    struct desk_folder *f=d.sh.folder; assert(f->busy && f->dialog && f->dialog_window>=0);
    assert(!strcmp(f->dialog->dialog.title,"Copying...") && strstr(f->dialog->dialog.text,"Preparing the selected items") && f->transfer);
    uint64_t latency=0, spent=folder_clock(FOLDER_CPU_CLOCK), begin=folder_clock(CLOCK_MONOTONIC);
    for (unsigned k=0;k<120;k++) {
        uint64_t at=folder_clock(CLOCK_MONOTONIC); d.now_ns+=1000000;
        struct ciuki_input_event motion={.type=CIUKI_INPUT_MOTION,.value=500+(int)(k%20),.value2=500}; desk_input(&d,&motion);
        desk_folder_service(&d);
        uint64_t elapsed=folder_clock(CLOCK_MONOTONIC)-at; if(elapsed>latency)latency=elapsed;
        struct timespec pause={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    assert(strstr(f->dialog->dialog.text,"Bytes:"));
    spent=folder_clock(FOLDER_CPU_CLOCK)-spent; uint64_t elapsed=folder_clock(CLOCK_MONOTONIC)-begin;
    assert(latency<50000000ull && spent*100/elapsed<=5 && f->busy);
    printf("DESKTOP TRANSFER input_max_ms=%.3f ui_cpu_pct=%.3f turns=120\n",latency/1e6,spent*100.0/elapsed);
    /* Pin the shell-hosted, shared dialog body for both themes. Worker stays
     * blocked; snapshots below are UI-owned test presentation inputs. */
    struct transfer_view view={.state=FM_RUNNING,.kind=FM_COPY,.bytes=3145728,.total=10485760,.file_bytes=3145728,.file_total=10485760,.rate=1048576,.items=1,.count=4};
    strcpy(view.source,"/mnt/e/large.mp3"); strcpy(view.target,DESK_FOLDER "/large.mp3");
    for(unsigned theme=0;theme<2;theme++) {
        assert(!desk_theme_use(desk_theme_at(theme)->name));
        for(unsigned kind=FM_COPY;kind<=FM_DELETE;kind++) {
            view.kind=(enum fm_job_kind)kind; view.state=FM_RUNNING;
            pthread_mutex_lock(&f->transfer->lock); f->transfer->view=view; pthread_mutex_unlock(&f->transfer->lock);
            f->transfer->started_at=job_now()-3000000000ull; f->progress_ns=0; desk_folder_service(&d);
            char name[64]; snprintf(name,sizeof(name),"desktop-transfer-%u-%u",theme,kind); capture(name);
        }
        view.existing=(struct fm_meta){.type=FM_FILE,.size=100}; view.meta=(struct fm_meta){.type=FM_FILE,.size=200};
        view.state=FM_REPLACE;
        pthread_mutex_lock(&f->transfer->lock); f->transfer->view=view; pthread_mutex_unlock(&f->transfer->lock);
        f->progress_ns=0; desk_folder_service(&d);
        char name[64]; snprintf(name,sizeof(name),"desktop-replace-%u",theme); capture(name);
    }
    assert(!desk_theme_use(desk_theme_at(0)->name)); f->display_kind=FM_COPY;
    desk_folder_cancel(&d);
    pthread_mutex_lock(&poll_lock); copy_release=1; pthread_cond_broadcast(&poll_changed); pthread_mutex_unlock(&poll_lock);
    settle(); assert(f->job.state==FM_CANCELLED && fake_lookup(DESK_FOLDER "/Worker.mp3")<0); fake_assert_clean();
    /* A real collision sleeps on the mailbox. No automatic Cancel. */
    fake_file(DESK_FOLDER "/Worker.mp3",10,90); command(DF_REFRESH,-1); command(DF_PASTE,-1);
    for(unsigned k=0;f->dialog->dialog.id!=851;k++) {
        assert(k<3000); d.now_ns+=1000000; desk_folder_service(&d); struct timespec pause={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    assert(f->busy && f->dialog->dialog.buttons==4);
    /* No leaves the destination unchanged; then Yes to all replaces it. */
    desk_folder_dialog_key(&d,DU_KEY_RIGHT,1); desk_folder_dialog_key(&d,DU_KEY_RIGHT,0);
    desk_folder_dialog_key(&d,DU_KEY_RIGHT,1); desk_folder_dialog_key(&d,DU_KEY_RIGHT,0);
    desk_folder_dialog_key(&d,DU_KEY_ENTER,1); desk_folder_dialog_key(&d,DU_KEY_ENTER,0);
    settle(); assert(f->job.state==FM_DONE && f->job.skipped && fake_nodes[fake_lookup(DESK_FOLDER "/Worker.mp3")].meta.size==10);
    command(DF_PASTE,-1);
    for(unsigned k=0;f->dialog->dialog.id!=851;k++) { assert(k<3000); d.now_ns+=1000000; desk_folder_service(&d); struct timespec pause={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&pause,NULL); }
    struct du_rect button=du_dialog_button(f->dialog,1); struct desk_box content=desk_content(&d.clients[f->dialog_window]);
    desk_folder_dialog_click(&d,content.x+button.x-f->dialog->dialog.box.x-DESK_BORDER+3,content.y+button.y-f->dialog->dialog.box.y-DESK_TITLE+3);
    settle(); assert(f->job.state==FM_DONE && !f->job.skipped && fake_nodes[fake_lookup(DESK_FOLDER "/Worker.mp3")].meta.size==30000);
    puts("PASS desktop transfer: shell input during blocked I/O, shared dialogs, No/Yes-to-all, cancel cleanup");
}
