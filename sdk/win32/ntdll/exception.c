/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Original x86 SEH implementation. */
#include <w32/ntdll.h>
#include <w32/shims.h>
#define END_CHAIN ((struct w32_seh *)~(uintptr_t)0)
struct nested_frame {
    struct w32_seh registration;
    struct w32_seh *protected;
    int unwind;
};
static int valid_frame(struct w32_seh *f) {
    struct w32_tib *t = &NtCurrentTeb()->Tib;
    uintptr_t a = (uintptr_t)f, lo = (uintptr_t)t->StackLimit, hi = (uintptr_t)t->StackBase;
    return !(a & 3) && a >= lo && a <= hi && hi - a >= sizeof(*f);
}
static LONG nested_handler(struct w32_exception *r, struct w32_seh *f, struct w32_context *c,
                           struct w32_seh **dispatch) {
    (void)c;
    struct nested_frame *n = (struct nested_frame *)f;
    int unwind = (r->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) != 0;
    if (unwind != n->unwind)
        return ExceptionContinueSearch;
    *dispatch = n->protected;
    return unwind ? ExceptionCollidedUnwind : ExceptionNestedException;
}
static LONG invoke(struct w32_exception *r, struct w32_seh *f, struct w32_context *c,
                   struct w32_seh **dispatch, int unwind) {
    struct w32_teb *t = NtCurrentTeb();
    struct nested_frame n = {{t->Tib.ExceptionList, nested_handler}, f, unwind};
    t->Tib.ExceptionList = &n.registration;
    LONG result = f->Handler(r, f, c, dispatch);
    /* A collided unwind may have already removed the guard. */
    if (t->Tib.ExceptionList == &n.registration)
        t->Tib.ExceptionList = n.registration.Prev;
    return result;
}
NTSTATUS w32_dispatch_exception(struct w32_exception *r, struct w32_context *c) {
    if (!r || !c || r->NumberParameters > 15)
        return STATUS_INVALID_PARAMETER;
    struct w32_seh *f = NtCurrentTeb()->Tib.ExceptionList, *nested = 0;
    for (ULONG count = 0; f != END_CHAIN && count < 4096; count++) {
        if (!valid_frame(f) || !f->Handler) {
            r->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            return STATUS_BAD_STACK;
        }
        struct w32_seh *dispatch = 0, *next = f->Prev;
        LONG result = invoke(r, f, c, &dispatch, 0);
        if (f == nested) {
            nested = 0;
            r->ExceptionFlags &= ~EXCEPTION_NESTED_CALL;
        }
        switch (result) {
        case ExceptionContinueExecution:
            return r->ExceptionFlags & EXCEPTION_NONCONTINUABLE ? STATUS_NONCONTINUABLE_EXCEPTION
                                                                : 0;
        case ExceptionContinueSearch:
            break;
        case ExceptionNestedException:
            if (!valid_frame(dispatch) || (uintptr_t)dispatch < (uintptr_t)f)
                return STATUS_INVALID_DISPOSITION;
            if (!nested || (uintptr_t)dispatch > (uintptr_t)nested)
                nested = dispatch;
            r->ExceptionFlags |= EXCEPTION_NESTED_CALL;
            break;
        default:
            return STATUS_INVALID_DISPOSITION;
        }
        if (next != END_CHAIN && (uintptr_t)next <= (uintptr_t)f) {
            r->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            return STATUS_BAD_STACK;
        }
        f = next;
    }
    if (f != END_CHAIN)
        return STATUS_BAD_STACK;
    return STATUS_UNHANDLED_EXCEPTION;
}
BOOLEAN W32API RtlDispatchException(struct w32_exception *r, struct w32_context *c) {
    return !w32_dispatch_exception(r, c);
}
NTSTATUS w32_unwind(struct w32_seh *target, struct w32_exception *r, struct w32_context *c) {
    struct w32_teb *t = NtCurrentTeb();
    struct w32_seh *f = t->Tib.ExceptionList;
    r->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (!target)
        r->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;
    if (target && !valid_frame(target))
        return STATUS_INVALID_UNWIND_TARGET;
    for (ULONG count = 0; f != END_CHAIN && count < 4096; count++) {
        if (!valid_frame(f) || !f->Handler)
            return STATUS_BAD_STACK;
        if (target && (uintptr_t)f > (uintptr_t)target)
            return STATUS_INVALID_UNWIND_TARGET;
        if (f == target)
            return 0;
        struct w32_seh *dispatch = 0, *next = f->Prev;
        LONG result = invoke(r, f, c, &dispatch, 1);
        if (result == ExceptionCollidedUnwind) {
            if (!valid_frame(dispatch) || (uintptr_t)dispatch < (uintptr_t)f)
                return STATUS_INVALID_DISPOSITION;
            if (target && (uintptr_t)dispatch > (uintptr_t)target)
                return STATUS_INVALID_UNWIND_TARGET;
            if (dispatch == target) {
                t->Tib.ExceptionList = target;
                return 0;
            }
            f = dispatch;
            next = f->Prev;
            r->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
        } else if (result != ExceptionContinueSearch)
            return STATUS_INVALID_DISPOSITION;
        if (next != END_CHAIN && (uintptr_t)next <= (uintptr_t)f)
            return STATUS_BAD_STACK;
        t->Tib.ExceptionList = next;
        f = next;
        r->ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
    }
    if (f != END_CHAIN)
        return STATUS_BAD_STACK;
    return target ? STATUS_INVALID_UNWIND_TARGET : 0;
}
static W32_NORETURN void resume(struct w32_context *c) {
    const struct w32_services *s = w32_get_services();
    if (s && s->continue_context)
        s->continue_context(s->opaque, c);
    w32_terminate(STATUS_NOT_IMPLEMENTED);
}
static w32_top_filter top_filter;
w32_top_filter W32API SetUnhandledExceptionFilter(w32_top_filter filter) {
    return __atomic_exchange_n(&top_filter, filter, __ATOMIC_ACQ_REL);
}
LONG W32API UnhandledExceptionFilter(struct w32_exception_pointers *p) {
    w32_top_filter filter = __atomic_load_n(&top_filter, __ATOMIC_ACQUIRE);
    struct w32_teb *t = NtCurrentTeb();
    if (!p || !p->ExceptionRecord || !p->ContextRecord || !filter || t->CoreFilterDepth)
        return 0;
    t->CoreFilterDepth++;
    LONG result = filter(p);
    t->CoreFilterDepth--;
    return result;
}
NTSTATUS w32_dispatch_user_exception(struct w32_exception *r, struct w32_context *c) {
    NTSTATUS status = w32_dispatch_exception(r, c);
    if (status == STATUS_UNHANDLED_EXCEPTION) {
        struct w32_exception_pointers p = {r, c};
        LONG result = UnhandledExceptionFilter(&p);
        if (result == -1)
            return r->ExceptionFlags & EXCEPTION_NONCONTINUABLE ? STATUS_NONCONTINUABLE_EXCEPTION
                                                                : 0;
        if (result != 0 && result != 1)
            return STATUS_INVALID_DISPOSITION;
    }
    return status;
}
/* Windows semantics for a handler that continues a noncontinuable exception
 * or returns an invalid disposition: raise STATUS_NONCONTINUABLE_EXCEPTION or
 * STATUS_INVALID_DISPOSITION as a new noncontinuable exception whose
 * ExceptionRecord links the original, so outer handlers can see both. The
 * record chain bounds the recursion of a handler that keeps continuing. */
static W32_NORETURN void finish(struct w32_exception *r, struct w32_context *c, NTSTATUS status) {
    if (!status)
        resume(c);
    if (status == STATUS_NONCONTINUABLE_EXCEPTION || status == STATUS_INVALID_DISPOSITION) {
        ULONG depth = 0;
        for (struct w32_exception *e = r; e && depth < 16; e = e->ExceptionRecord)
            depth++;
        if (depth < 8) {
            struct w32_exception nested;
            w32_fill(&nested, 0, sizeof(nested));
            nested.ExceptionCode = status;
            nested.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
            nested.ExceptionRecord = r;
            RtlRaiseException(&nested);
        }
    }
    w32_report_exception(r,c);
    w32_terminate(status == STATUS_UNHANDLED_EXCEPTION ? r->ExceptionCode : (ULONG)status);
}
/* Kernel (or host adapter) delivery entry: record and context on the user
 * stack below the interrupted ESP, then EIP here with both pointers. */
void W32API KiUserExceptionDispatcher(struct w32_exception *r, struct w32_context *c) {
    finish(r, c, w32_dispatch_user_exception(r, c));
}
/* Assembly captures the caller's nonvolatile registers before C uses them. */
void W32API w32_raise_c(struct w32_exception *r, struct w32_context *c) {
    if (r->NumberParameters > 15)
        r->NumberParameters = 15;
    r->ExceptionAddress = (void *)(uintptr_t)c->Eip;
    const struct w32_services *s=w32_get_services();
    /* Linked nested records are already inside the user dispatcher; the
     * frozen RAISE wire record deliberately accepts an unlinked first raise. */
    if(!r->ExceptionRecord && s && s->raise && s->raise(s->opaque,r,c))
        w32_terminate(STATUS_INVALID_PARAMETER); /* successful delivery replaces EIP */
    finish(r, c, w32_dispatch_user_exception(r, c));
}
struct w32_teb *W32API w32_current_teb(void) {
    return NtCurrentTeb();
}
void W32API RaiseException(ULONG code, ULONG flags, ULONG count, const ULONG *args) {
    struct w32_exception r;
    w32_fill(&r, 0, sizeof(r));
    r.ExceptionCode = code & ~0x10000000u;
    r.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
    r.NumberParameters = count > 15 ? 15 : count;
    if (args)
        w32_copy(r.ExceptionInformation, args, r.NumberParameters * 4);
    RtlRaiseException(&r);
}
W32_NORETURN void W32API RtlRaiseStatus(NTSTATUS status) {
    struct w32_exception r;
    w32_fill(&r, 0, sizeof(r));
    r.ExceptionCode = status;
    r.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    RtlRaiseException(&r);
    w32_terminate(status);
}
void W32API w32_unwind_c(void *target, void *ip, struct w32_exception *record, void *retval,
                         struct w32_context *c) {
    struct w32_exception local;
    if (!record) {
        w32_fill(&local, 0, sizeof(local));
        local.ExceptionCode = STATUS_UNWIND;
        local.ExceptionAddress = (void *)(uintptr_t)c->Eip;
        record = &local;
    }
    NTSTATUS status = w32_unwind(target, record, c);
    if (status)
        RtlRaiseStatus(status);
    if (ip)
        c->Eip = (ULONG)(uintptr_t)ip;
    c->Eax = (ULONG)(uintptr_t)retval;
    resume(c);
}
