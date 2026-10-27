/* Production credential predicates with admission faked at the boundary.
 * No capability-bit or pathname authorization model. GPL-2.0-only */
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t flags) { (void)flags; }
#include "../../../src/kernel/proc/security.c"
#include "subject_fixture.h"
#include <assert.h>
static struct security_subject fixture_subjects[32];
static struct process *fixture_processes[32];
struct process *proc_reserved_at_slot(unsigned slot) __attribute__((weak));
struct process *proc_reserved_at_slot(unsigned slot)
{ return slot<32 ? fixture_processes[slot] : 0; }
void security_fixture_identity(struct process *p, uint32_t uid, uint32_t session, bool admin, uint32_t role)
{
    unsigned i;
    for (i=0;i<32 && fixture_processes[i] && fixture_processes[i]!=p;i++);
    assert(i<32);
    fixture_processes[i]=p;
    struct security_subject *s=&fixture_subjects[i];
    *s=(struct security_subject){.refs=1,.live=true,.pid=p->pid,.role=role,.process_generation=1,
        .credential={.version=1,.size=64,.uid=uid,.session_id=session,
            .flags=session ? CIUKI_CRED_INTERACTIVE : 0}};
    if (!uid) { s->credential.primary_gid=0; s->credential.group_count=1; }
    else if (uid==501) { s->credential.primary_gid=546; s->credential.group_count=1; s->credential.groups[0]=546; }
    else { s->credential.primary_gid=545; s->credential.group_count=admin ? 2 : 1;
        s->credential.groups[0]=admin ? 544 : 545; s->credential.groups[1]=admin ? 545 : 0; }
    p->subject=s;
}
