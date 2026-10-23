/* fx-5: call 73's native barrier (production power_quiesce.c) under the
 * kmutex order rule of sync.h, after the T23 trial of image 9a60215f
 * panicked `mutex: lock order` in audio_power_quiesce. Network and USB are
 * modeled by their sequential acquisitions (net.c locks the registration
 * mutex, then each device mutex, never nested; USB uses fs_lock tickets);
 * their own suites test their stop proofs. */
#include "../../../src/kernel/core/power_quiesce.c"
static struct { struct kmutex registration, device; } net_order_model;
static char quiesce_order[8];
int net_power_quiesce(void)
{
    kmutex_lock(&net_order_model.registration); kmutex_unlock(&net_order_model.registration);
    kmutex_lock(&net_order_model.device); kmutex_unlock(&net_order_model.device);
    strcat(quiesce_order,"N"); return 0;
}
int usb_power_quiesce(void) { strcat(quiesce_order,"U"); return 0; }
static jmp_buf worker_turn;
static void worker_turn_end(void) { longjmp(worker_turn,1); }
static void test_shutdown_lock_order(void)
{
    /* Negative control: the trial layout, the outer (service) mutex at the
     * higher address. The model must report it, naming both locks. */
    static struct { struct kmutex lower, higher; } trial;
    kmutex_init_named(&trial.lower,"audio.control"); kmutex_init_named(&trial.higher,"audio.service");
    unsigned violations=lock_order_violations; lock_order_expected=true;
    kmutex_lock(&trial.higher); kmutex_lock(&trial.lower);
    lock_order_expected=false;
    CHECK(lock_order_violations==violations+1);
    CHECK(strstr(lock_order_record,"lock order want=audio.control/") && strstr(lock_order_record,"held=audio.service/"));
    kmutex_unlock(&trial.lower); kmutex_unlock(&trial.higher);
    lock_order_violations=violations;
    CHECK((uintptr_t)&audio_lock.service<(uintptr_t)&audio_lock.control);
    /* Outer level: nesting the backend's register lock or the EC lock holds
     * whichever side of them the linker (or this host TU) places the struct. */
    audio_locks_init();
    CHECK(audio_lock.service.level==KMUTEX_LEVEL_OUTER && audio_lock.control.level==KMUTEX_LEVEL_OUTER);
    CHECK(kmutex_order_conflict(&audio_lock.control,&audio_lock.service));
    CHECK(!kmutex_order_conflict(&audio_lock.control,&audio_dma.register_lock) &&
          !kmutex_order_conflict(&audio_lock.service,&audio_dma.register_lock));
    CHECK(kmutex_order_conflict(&audio_dma.register_lock,&audio_lock.service));

    unsigned before=pages_used;hardware_setup(&ac97);
    resource_releases=resource_quarantines=irq_removals=0;
    for(unsigned i=0;i<12;i++) {
        ac97.claims[i]=i;ac97.claim_generation[i]=100+i;
        audio_resources[i]=(struct resource){.generation=100+i,.state=RS_CLAIMED};
    }
    ac97.bdl_phys=pmm_alloc();CHECK(ac97.bdl_phys);ac97.bdl=P2V(ac97.bdl_phys);
    for(unsigned i=0;i<8;i++){ac97.payload_pages[i]=pmm_alloc();CHECK(ac97.payload_pages[i]);}
    audio.commitment_pages=9;ac97.ready=true;
    audio.initialized=true;power_stopped=false;int saved_error=audio.error;audio.error=0;
    audio_locks_init();
    kmutex_init_named(&net_order_model.registration,"net.registration"); kmutex_init_named(&net_order_model.device,"netif.mutex");
    /* The ThinkPad EC quirk is live, so its stop takes ec_lock under both. */
    kmutex_init_named(&ec_lock,"thinkpad.ec"); hec.io.now=ec_mock_io().now;
    unsigned acquisitions=lock_acquisitions; quiesce_order[0]=0;
    CHECK(!power_devices_quiesce() && !strcmp(quiesce_order,"NU") && power_stopped && !hec.io.now);
    CHECK(lock_acquisitions-acquisitions==5 && !lock_order_violations && !held_mutexes);
    CHECK(resource_releases==12 && pages_used==before && !audio.commitment_pages && audio.error==-ENODEV);
    /* The worker's error turn after the barrier nests service, then control. */
    acquisitions=lock_acquisitions; on_schedule=worker_turn_end;
    if (!setjmp(worker_turn)) { audio_worker(0); CHECK(false); }
    on_schedule=0; g_current->state=T_RUNNING; /* the turn ended inside its sleep */
    CHECK(lock_acquisitions-acquisitions==2 && !lock_order_violations && !held_mutexes);
    /* A repeated barrier (second call-73 request) is idempotent. */
    acquisitions=lock_acquisitions; quiesce_order[0]=0;
    CHECK(!power_devices_quiesce() && !strcmp(quiesce_order,"NU") && lock_acquisitions-acquisitions==2);
    memset(&ac97,0,sizeof(ac97)); memset(&hec,0,sizeof(hec));
    power_stopped=false; audio.error=saved_error;
    puts("audio shutdown order: call-73 barrier net/audio/EC/usb and the worker error turn hold sync.h (level, address) order (service<control, outer to DMA register and EC locks); trial layout negative control names both locks PASS");
}
