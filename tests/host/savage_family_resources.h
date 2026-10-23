/* Extra fake-bus boundary for the family fixture; no production test hook.
 * SPDX-License-Identifier: GPL-2.0-only */
static bool family_resources;
static int family_claim(enum res_type,uint32_t,uint32_t,const char *,bool);
static const struct resource *family_get(unsigned);
static int family_activate(int,gen_t);
static int family_quarantine(int,gen_t);
static int family_quiesce(int,gen_t,registry_idle_proof_fn);
static int family_release(int,gen_t);
static int family_takeover(int,gen_t,const char *,registry_idle_proof_fn);
static void *family_reserve(uint32_t);
static void *family_map(void *,uint32_t,uint32_t,uint32_t);
static bool family_marker_path(const char *);
