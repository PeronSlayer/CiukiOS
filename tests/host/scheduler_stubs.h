/* Scheduling boundary for isolated driver fixtures. Effective priorities are
 * exercised with production task.c in scheduler_priorities_test.c. */
void task_set_inherited(struct task *t, unsigned p)
{ t->inherited_priority = p; }
void task_io_completed(struct task *t) { (void)t; }
void task_set_share_lent(struct task *t, bool lent) { t->share_lent = lent; }
