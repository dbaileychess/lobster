// Copyright 2014 Wouter van Oortmerssen. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lobster/stdafx.h"

#include "lobster/disasm.h"

#include "lobster/vmops.h"

namespace lobster {

enum {
    // *8 bytes each
    INITSTACKSIZE   =  32 * 1024,
    // *8 bytes each, modest on smallest handheld we support (iPhone 3GS has 256MB).
    DEFMAXSTACKSIZE = 512 * 1024,
    // *8 bytes each, max by which the stack could possibly grow in a single call.
    STACKMARGIN     =   8 * 1024
};

VM::VM(VMArgs &&vmargs, const bytecode::BytecodeFile *bcf)
    : VMArgs(std::move(vmargs)), maxstacksize(DEFMAXSTACKSIZE), bcf(bcf) {

    codelen = bcf->bytecode()->Length();
    if (FLATBUFFERS_LITTLEENDIAN) {
        // We can use the buffer directly.
        codestart = (const int *)bcf->bytecode()->Data();
        typetable = (const type_elem_t *)bcf->typetable()->Data();
    } else {
        for (uint32_t i = 0; i < codelen; i++)
            codebigendian.push_back(bcf->bytecode()->Get(i));
        codestart = codebigendian.data();

        for (uint32_t i = 0; i < bcf->typetable()->Length(); i++)
            typetablebigendian.push_back((type_elem_t)bcf->typetable()->Get(i));
        typetable = typetablebigendian.data();
    }
    stack = new Value[stacksize = INITSTACKSIZE];
    constant_strings.resize(bcf->stringtable()->size());
    assert(native_vtables);
}

VM::~VM() {
    TerminateWorkers();
    if (stack) delete[] stack;
    if (byteprofilecounts) delete[] byteprofilecounts;
}

VMAllocator::VMAllocator(VMArgs &&args) {
    // Verify the bytecode.
    flatbuffers::Verifier verifier(args.static_bytecode, args.static_size);
    auto ok = bytecode::VerifyBytecodeFileBuffer(verifier);
    if (!ok) THROW_OR_ABORT("bytecode file failed to verify");
    auto bcf = bytecode::GetBytecodeFile(args.static_bytecode);
    if (bcf->bytecode_version() != LOBSTER_BYTECODE_FORMAT_VERSION)
        THROW_OR_ABORT("bytecode is from a different version of Lobster");

    // Allocate enough memory to fit the "vars" array inline.
    auto size = sizeof(VM) + sizeof(Value) * bcf->specidents()->size();
    auto mem = malloc(size);
    assert(mem);
    memset(mem, 0, size);  // FIXME: this shouldn't be necessary.

    #undef new

    vm = new (mem) VM(std::move(args), bcf);

    #ifdef _WIN32
    #ifndef NDEBUG
    #define new DEBUG_NEW
    #endif
    #endif
}

VMAllocator::~VMAllocator() {
    if (!vm) return;
    vm->~VM();
    free(vm);
}

const TypeInfo &VM::GetVarTypeInfo(int varidx) {
    return GetTypeInfo((type_elem_t)bcf->specidents()->Get(varidx)->typeidx());
}

type_elem_t VM::GetIntVectorType(int which) {
    auto i = bcf->default_int_vector_types()->Get(which);
    return type_elem_t(i < 0 ? -1 : i);
}
type_elem_t VM::GetFloatVectorType(int which) {
    auto i = bcf->default_float_vector_types()->Get(which);
    return type_elem_t(i < 0 ? -1 : i);
}

static bool _LeakSorter(void *va, void *vb) {
    auto a = (RefObj *)va;
    auto b = (RefObj *)vb;
    return a->refc != b->refc
    ? a->refc > b->refc
    : (a->tti != b->tti
        ? a->tti > b->tti
        : false);
}

void VM::DumpVal(RefObj *ro, const char *prefix) {
    string sd;
    append(sd, prefix, ": ");
    RefToString(*this, sd, ro, debugpp);
    append(sd, " (", ro->refc, "): ", (size_t)ro);
    LOG_DEBUG(sd);
}

void VM::DumpFileLine(const int *fip, string &sd) {
    // error is usually in the byte before the current ip.
    auto li = LookupLine(fip - 1, codestart, bcf);
    append(sd, bcf->filenames()->Get(li->fileidx())->string_view(), "(", li->line(), ")");
}

void VM::DumpLeaks() {
    vector<void *> leaks = pool.findleaks();
    auto filename = "leaks.txt";
    if (leaks.empty()) {
        if (FileExists(filename)) FileDelete(filename);
    } else {
        LOG_ERROR("LEAKS FOUND (this indicates cycles in your object graph, or a bug in"
                             " Lobster)");
        sort(leaks.begin(), leaks.end(), _LeakSorter);
        PrintPrefs leakpp = debugpp;
        leakpp.cycles = 0;
        string sd;
        for (auto p : leaks) {
            auto ro = (RefObj *)p;
            switch(ro->ti(*this).t) {
                case V_VALUEBUF:
                case V_STACKFRAMEBUF:
                    break;
                case V_STRING:
                case V_RESOURCE:
                case V_VECTOR:
                case V_CLASS: {
                    ro->CycleStr(sd);
                    sd += " = ";
                    RefToString(*this, sd, ro, leakpp);
                    #if DELETE_DELAY
                    sd += " ";
                    DumpFileLine(ro->alloc_ip, sd);
                    append(sd, " ", (size_t)ro);
                    #endif
                    sd += "\n";
                    break;
                }
                default: assert(false);
            }
        }
        #ifndef NDEBUG
            LOG_ERROR(sd);
        #else
            if (leaks.size() < 50) {
                LOG_ERROR(sd);
            } else {
                LOG_ERROR(leaks.size(), " leaks, details in ", filename);
                WriteFile(filename, false, sd);
            }
        #endif
    }
    pool.printstats(false);
}

void VM::OnAlloc(RefObj *ro) {
    #if DELETE_DELAY
        LOG_DEBUG("alloc: ", (size_t)ro, " - ", ro->refc);
        ro->alloc_ip = ip;
    #else
        (void)ro;
    #endif
}

#undef new

LVector *VM::NewVec(iint initial, iint max, type_elem_t tti) {
    assert(GetTypeInfo(tti).t == V_VECTOR);
    auto v = new (pool.alloc_small(sizeof(LVector))) LVector(*this, initial, max, tti);
    OnAlloc(v);
    return v;
}

LObject *VM::NewObject(iint max, type_elem_t tti) {
    assert(IsUDT(GetTypeInfo(tti).t));
    auto s = new (pool.alloc(ssizeof<LObject>() + ssizeof<Value>() * max)) LObject(tti);
    OnAlloc(s);
    return s;
}

LString *VM::NewString(iint l) {
    auto s = new (pool.alloc(ssizeof<LString>() + l + 1)) LString(l);
    OnAlloc(s);
    return s;\
}

LResource *VM::NewResource(void *v, const ResourceType *t) {
    auto r = new (pool.alloc(sizeof(LResource))) LResource(v, t);
    OnAlloc(r);
    return r;
}

#ifdef _WIN32
#ifndef NDEBUG
#define new DEBUG_NEW
#endif
#endif

LString *VM::NewString(string_view s) {
    auto r = NewString(s.size());
    auto dest = (char *)r->data();
    memcpy(dest, s.data(), s.size());
    #if DELETE_DELAY
        LOG_DEBUG("string: \"", s, "\" - ", (size_t)r);
    #endif
    return r;
}

LString *VM::NewString(string_view s1, string_view s2) {
    auto s = NewString(s1.size() + s2.size());
    auto dest = (char *)s->data();
    memcpy(dest, s1.data(), s1.size());
    memcpy(dest + s1.size(), s2.data(), s2.size());
    return s;
}

LString *VM::ResizeString(LString *s, iint size, int c, bool back) {
    auto ns = NewString(size);
    auto sdest = (char *)ns->data();
    auto cdest = sdest;
    auto remain = size - s->len;
    if (back) sdest += remain;
    else cdest += s->len;
    memcpy(sdest, s->data(), (size_t)s->len);
    memset(cdest, c, (size_t)remain);
    s->Dec(*this);
    return ns;
}

void VM::ErrorBase(const string &err) {
    if (error_has_occured) {
        // We're calling this function recursively, not good. Try to get back to a reasonable
        // state by throwing an exception to be caught by the original error.
        errmsg = err;
        UnwindOnError();
    }
    error_has_occured = true;
    if (trace == TraceMode::TAIL && trace_output.size()) {
        for (size_t i = trace_ring_idx; i < trace_output.size(); i++) errmsg += trace_output[i];
        for (size_t i = 0; i < trace_ring_idx; i++) errmsg += trace_output[i];
        errmsg += err;
        UnwindOnError();
    }
    append(errmsg, "VM error: ", err);
}

// This function is now way less important than it was when the language was still dynamically
// typed. But ok to leave it as-is for "index out of range" and other errors that are still dynamic.
Value VM::Error(StackPtr sp, string err) {
    ErrorBase(err);
    #ifdef USE_EXCEPTION_HANDLING
    try {
    #endif
        while (sp >= stack && (!stackframes.size() || sp - stack != stackframes.back().spstart)) {
            // Sadly can't print this properly.
            errmsg += "\n   stack: ";
            to_string_hex(errmsg, (size_t)Top(sp).any());
            if (pool.pointer_is_in_allocator(Top(sp).any())) {
                errmsg += ", maybe: ";
                RefToString(*this, errmsg, Top(sp).ref(), debugpp);
            }
            Pop(sp);  // We don't DEC here, as we can't know what type it is.
                    // This is ok, as we ignore leaks in case of an error anyway.
        }
        for (;;) {
            if (!stackframes.size()) break;
            int deffun = *(stackframes.back().funstart);
            if (deffun >= 0) {
                append(errmsg, "\nin function: ", bcf->functions()->Get(deffun)->name()->string_view());
            } else {
                errmsg += "\nin block";
            }
            auto &stf = stackframes.back();
            auto fip = stf.funstart;
            fip++;  // function id.
            auto nargs = *fip++;
            auto freevars = fip + nargs;
            fip += nargs;
            auto ndef = *fip++;
            fip += ndef;
            auto defvars = fip;
            auto nkeepvars = *fip++;
            if (errmsg.size() < 10000) {
                // FIXME: merge with loops below.
                for (int j = 0; j < ndef; ) {
                    auto i = *(defvars - j - 1);
                    j += DumpVar(errmsg, vars[i], i);
                }
                for (int j = 0; j < nargs; ) {
                    auto i = *(freevars - j - 1);
                    j += DumpVar(errmsg, vars[i], i);
                }
            }
            sp -= nkeepvars;
            fip++;  // Owned vars.
            while (ndef--) {
                auto i = *--defvars;
                vars[i] = Pop(sp);
            }
            while (nargs--) {
                auto i = *--freevars;
                vars[i] = Pop(sp);
            }
            stackframes.pop_back();
            sp = (stackframes.size() ? stackframes.back().spstart : -1) + stack;
        }
    #ifdef USE_EXCEPTION_HANDLING
    } catch (string &s) {
        // Error happened while we were building this stack trace.
        append(errmsg, "\nRECURSIVE ERROR:\n", s);
    }
    #endif
    UnwindOnError();
    return Value();
}

// Unlike Error above, this one does not attempt any variable dumping since the VM may already be
// in an inconsistent state.
Value VM::SeriousError(string err) {
    ErrorBase(err);
    UnwindOnError();
    return Value();
}

void VM::VMAssert(const char *what)  {
    SeriousError(string("VM internal assertion failure: ") + what);
}

int VM::DumpVar(string &sd, const Value &x, int idx) {
    auto sid = bcf->specidents()->Get((uint32_t)idx);
    auto id = bcf->idents()->Get(sid->ididx());
    // FIXME: this is not ideal, it filters global "let" declared vars.
    // It should probably instead filter global let vars whose values are entirely
    // constructors, and which are never written to.
    if (id->readonly() && id->global()) return 1;
    auto name = id->name()->string_view();
    auto &ti = GetVarTypeInfo(idx);
    #if RTT_ENABLED
        if (ti.t != x.type) return 1;  // Likely uninitialized.
    #endif
    append(sd, "\n   ", name, " = ");
    if (IsStruct(ti.t)) {
        StructToString(sd, debugpp, ti, &x);
        return ti.len;
    } else {
        x.ToString(*this, sd, ti, debugpp);
        return 1;
    }
}

void VM::FinalStackVarsCleanup(StackPtr &sp) {
    VMASSERT((*this), sp == stack - 1 && !stackframes.size());
    #ifndef NDEBUG
        LOG_INFO("stack at its highest was: ", maxsp);
    #else
        (void)sp;
    #endif
}

// Only valid to be called right after StartStackFrame, with no bytecode in-between.
void VM::FunIntro(StackPtr &sp, const int *ip) {
    stackframes.push_back(StackFrame());
    auto funstart = ip;
    ip++;  // definedfunction
    if (sp - stack > stacksize - STACKMARGIN) {
        // per function call increment should be small
        // FIXME: not safe for untrusted scripts, could simply add lots of locals
        // could record max number of locals? not allow more than N locals?
        if (stacksize >= maxstacksize)
            SeriousError("stack overflow! (use set_max_stack_size() if needed)");
        auto nstack = new Value[stacksize *= 2];
        t_memcpy(nstack, stack, sp - stack + 1);
        sp = sp - stack + nstack;
        delete[] stack;
        stack = nstack;


        LOG_DEBUG("stack grew to: ", stacksize);
    }
    auto nargs_fun = *ip++;
    for (int i = 0; i < nargs_fun; i++) swap(vars[ip[i]], *(sp - nargs_fun + i + 1));
    ip += nargs_fun;
    auto ndef = *ip++;
    for (int i = 0; i < ndef; i++) {
        // for most locals, this just saves an nil, only in recursive cases it has an actual value.
        auto varidx = *ip++;
        Push(sp, vars[varidx]);
        vars[varidx] = Value();
    }
    auto nkeepvars = *ip++;
    for (int i = 0; i < nkeepvars; i++) Push(sp, Value());
    auto nownedvars = *ip++;
    ip += nownedvars;
    auto &stf = stackframes.back();
    stf.funstart = funstart;
    stf.spstart = sp - stack;
    #ifndef NDEBUG
        if (sp - stack > maxsp) maxsp = sp - stack;
    #endif
}

void VM::FunOut(StackPtr &sp, int nrv) {
    sp -= nrv;
    // This is ok, since we don't push any values below.
    auto rets = TopPtr(sp);
    // This is guaranteed by the typechecker.
    assert(stackframes.size());
    auto &stf = stackframes.back();
    auto depth = sp - stack;
    if (depth != stf.spstart) {
        VMASSERT((*this), false);
    }
    auto fip = stf.funstart;
    fip++;  // function id.
    auto nargs = *fip++;
    auto freevars = fip + nargs;
    fip += nargs;
    auto ndef = *fip++;
    fip += ndef;
    auto defvars = fip;
    auto nkeepvars = *fip++;
    for (int i = 0; i < nkeepvars; i++) Pop(sp).LTDECRTNIL(*this);
    auto ownedvars = *fip++;
    for (int i = 0; i < ownedvars; i++) vars[*fip++].LTDECRTNIL(*this);
    while (ndef--) {
        auto i = *--defvars;
        vars[i] = Pop(sp);
    }
    while (nargs--) {
        auto i = *--freevars;
        vars[i] = Pop(sp);
    }
    stackframes.pop_back();
    ts_memcpy(TopPtr(sp), rets, nrv);
    sp += nrv;
}

void VM::EndEval(StackPtr &sp, const Value &ret, const TypeInfo &ti) {
    TerminateWorkers();
    ret.ToString(*this, evalret, ti, programprintprefs);
    ret.LTDECTYPE(*this, ti.t);
    #ifndef NDEBUG
        if (sp != stack - 1) {
            LOG_ERROR("stack diff: ", sp - stack - 1);
            while (sp >= stack - 1) {
                auto v = Pop(sp);
                LOG_ERROR("left on the stack: ", (size_t)v.any(), ", type: ", v.type);
            }
            assert(false);
        }
    #endif
    FinalStackVarsCleanup(sp);
    for (auto s : constant_strings) {
        if (s) s->Dec(*this);
    }
    while (!delete_delay.empty()) {
        auto ro = delete_delay.back();
        delete_delay.pop_back();
        ro->DECDELETENOW(*this);
    }
    DumpLeaks();
}

void VM::UnwindOnError() {
    // This is the single location from which we unwind the execution stack from within the VM.
    // This requires special care, because there may be jitted code on the stack, and depending
    // on the platform we can use exception handling, or not.
    // This code is only needed upon error, the regular execution path uses normal returns.
    #if VM_USE_LONGJMP
        // We are in JIT mode, and on a platform that cannot throw exceptions "thru" C code,
        // e.g. Linux.
        // To retain modularity (allow the VM to be used in an environment where a VM error
        // shouldn't terminate the whole app) we try to work around this with setjmp/longjmp.
        // This does NOT call destructors on the way, so code calling into here should make sure
        // to not require these.
        // Though even if there are some, a small memory leak upon a VM error is probably
        // preferable to aborting when modularity is needed.
        // FIXME: audit calling code for destructors. Can we automatically enforce this?
        longjmp(jump_buffer, 1);
        // The corresponding setjmp is right below here.
    #else
        // Use the standard error mechanism, which uses exceptions (on Windows, or other platforms
        // when not JIT-ing) or aborts (Wasm).
        THROW_OR_ABORT(errmsg);
    #endif
}

void VM::EvalProgram() {
    #if VM_USE_LONGJMP
        // See longjmp above for why this is needed.
        if (setjmp(jump_buffer)) {
            // Resume normal error now that we've jumped past the C/JIT-ted code.
            THROW_OR_ABORT(errmsg);
        }
    #endif
    auto sp = stack - 1;
    #if VM_JIT_MODE
        jit_entry(*this, sp);
    #else
        compiled_entry_point(*this, sp);
    #endif
}

string &VM::TraceStream() {
  size_t trace_size = trace == TraceMode::TAIL ? 50 : 1;
  if (trace_output.size() < trace_size) trace_output.resize(trace_size);
  if (trace_ring_idx == trace_size) trace_ring_idx = 0;
  auto &sd = trace_output[trace_ring_idx++];
  sd.clear();
  return sd;
}

string VM::ProperTypeName(const TypeInfo &ti) {
    switch (ti.t) {
        case V_STRUCT_R:
        case V_STRUCT_S:
        case V_CLASS: return string(ReverseLookupType(ti.structidx));
        case V_NIL: return ProperTypeName(GetTypeInfo(ti.subt)) + "?";
        case V_VECTOR: return "[" + ProperTypeName(GetTypeInfo(ti.subt)) + "]";
        case V_INT: return ti.enumidx >= 0 ? string(EnumName(ti.enumidx)) : "int";
        default: return string(BaseTypeName(ti.t));
    }
}

void VM::BCallRetCheck(StackPtr sp, const NativeFun *nf) {
    #if RTT_ENABLED
        // See if any builtin function is lying about what type it returns
        // other function types return intermediary values that don't correspond to final return
        // values.
        if (!nf->cont1) {
            for (size_t i = 0; i < nf->retvals.size(); i++) {
                #ifndef NDEBUG
                auto t = (TopPtr(sp) - nf->retvals.size() + i)->type;
                auto u = nf->retvals[i].type->t;
                assert(t == u || u == V_ANY || u == V_NIL || (u == V_VECTOR && IsUDT(t)));
                #endif
            }
            assert(nf->retvals.size() || Top(sp).type == V_NIL);
        }
    #else
        (void)nf;
    #endif
    (void)sp;
}

iint VM::GrabIndex(StackPtr &sp, int len) {
    auto &v = TopM(sp, len);
    for (len--; ; len--) {
        auto sidx = Pop(sp).ival();
        if (!len) return sidx;
        RANGECHECK((*this), sidx, v.vval()->len, v.vval());
        v = v.vval()->At(sidx);
    }
}

void VM::IDXErr(StackPtr sp, iint i, iint n, const RefObj *v) {
    string sd;
    append(sd, "index ", i, " out of range ", n, " of: ");
    RefToString(*this, sd, v, debugpp);
    Error(sp, sd);
}

string_view VM::StructName(const TypeInfo &ti) {
    return bcf->udts()->Get(ti.structidx)->name()->string_view();
}

string_view VM::ReverseLookupType(int v) {
    return bcf->udts()->Get((flatbuffers::uoffset_t)v)->name()->string_view();
}

bool VM::EnumName(string &sd, iint enum_val, int enumidx) {
    auto enum_def = bcf->enums()->Get(enumidx);
    auto &vals = *enum_def->vals();
    auto lookup = [&](iint val) -> bool {
        // FIXME: can store a bool that says wether this enum is contiguous, so we just index instead.
        for (auto v : vals)
            if (v->val() == val) {
                sd += v->name()->string_view();
                return true;
            }
        return false;
    };
    if (!enum_def->flags() || !enum_val) return lookup(enum_val);
    auto start = sd.size();
    auto upto = 64 - HighZeroBits(enum_val);
    for (int i = 0; i < upto; i++) {
        auto bit = enum_val & (1LL << i);
        if (bit) {
            if (sd.size() != start) sd += "|";
            if (!lookup(bit)) {
                // enum contains unknown bits, so can't display this properly.
                sd.resize(start);
                return false;
            }
        }
    }
    return true;
}

string_view VM::EnumName(int enumidx) {
    return bcf->enums()->Get(enumidx)->name()->string_view();
}

optional<int64_t> VM::LookupEnum(string_view name, int enumidx) {
    auto &vals = *bcf->enums()->Get(enumidx)->vals();
    for (auto v : vals)
        if (v->name()->string_view() == name)
            return v->val();
    return {};
}

void VM::StartWorkers(StackPtr &sp, iint numthreads) {
    if (is_worker) Error(sp, "workers can\'t start more worker threads");
    if (tuple_space) Error(sp, "workers already running");
    // Stop bad values from locking up the machine :)
    numthreads = min(numthreads, 256_L);
    tuple_space = new TupleSpace(bcf->udts()->size());
    for (iint i = 0; i < numthreads; i++) {
        // Create a new VM that should own all its own memory and be completely independent
        // from this one.
        // We share nfr and programname for now since they're fully read-only.
        // FIXME: have to copy bytecode buffer even though it is read-only.
        auto vmargs = *(VMArgs *)this;
        vmargs.program_args.resize(0);
        vmargs.trace = TraceMode::OFF;
        auto vma = new VMAllocator(std::move(vmargs));
        vma->vm->is_worker = true;
        vma->vm->tuple_space = tuple_space;
        workers.emplace_back([vma] {
            string err;
            #ifdef USE_EXCEPTION_HANDLING
            try
            #endif
            {
                vma->vm->EvalProgram();
            }
            #ifdef USE_EXCEPTION_HANDLING
            catch (string &s) {
                err = s;
            }
            #endif
            delete vma;
            // FIXME: instead return err to main thread?
            if (!err.empty()) LOG_ERROR("worker error: ", err);
        });
    }
}

void VM::TerminateWorkers() {
    if (is_worker || !tuple_space) return;
    tuple_space->alive = false;
    for (auto &tt : tuple_space->tupletypes) tt.condition.notify_all();
    for (auto &worker : workers) worker.join();
    workers.clear();
    delete tuple_space;
    tuple_space = nullptr;
}

void VM::WorkerWrite(StackPtr &sp, RefObj *ref) {
    if (!tuple_space) return;
    if (!ref) Error(sp, "thread write: nil reference");
    auto &ti = ref->ti(*this);
    if (ti.t != V_CLASS) Error(sp, "thread write: must be a class");
    auto st = (LObject *)ref;
    auto buf = new Value[ti.len];
    for (int i = 0; i < ti.len; i++) {
        // FIXME: lift this restriction.
        if (IsRefNil(GetTypeInfo(ti.elemtypes[i]).t))
            Error(sp, "thread write: only scalar class members supported for now");
        buf[i] = st->AtS(i);
    }
    auto &tt = tuple_space->tupletypes[ti.structidx];
    {
        unique_lock<mutex> lock(tt.mtx);
        tt.tuples.push_back(buf);
    }
    tt.condition.notify_one();
}

LObject *VM::WorkerRead(StackPtr &sp, type_elem_t tti) {
    auto &ti = GetTypeInfo(tti);
    if (ti.t != V_CLASS) Error(sp, "thread read: must be a class type");
    Value *buf = nullptr;
    auto &tt = tuple_space->tupletypes[ti.structidx];
    {
        unique_lock<mutex> lock(tt.mtx);
        tt.condition.wait(lock, [&] { return !tuple_space->alive || !tt.tuples.empty(); });
        if (!tt.tuples.empty()) {
            buf = tt.tuples.front();
            tt.tuples.pop_front();
        }
    }
    if (!buf) return nullptr;
    auto ns = NewObject(ti.len, tti);
    ns->Init(*this, buf, ti.len, false);
    delete[] buf;
    return ns;
}

}  // namespace lobster


// Make VM ops available as C functions for linking purposes:

extern "C" {

using namespace lobster;

void CVM_Trace(VM *vm, StackPtr sp, string op) {
    auto &sd = vm->TraceStream();
    sd += op;
    #if RTT_ENABLED
        if (sp >= vm->stack) {
            sd += " - ";
            Top(sp).ToStringBase(*vm, sd, Top(sp).type, vm->debugpp);
            if (sp > vm->stack) {
                sd += " - ";
                TopM(sp, 1).ToStringBase(*vm, sd, TopM(sp, 1).type, vm->debugpp);
            }
        }
    #else
        (void)sp;
    #endif
    // append(sd, " / ", (size_t)Top(sp).any());
    // for (int _i = 0; _i < 7; _i++) { append(sd, " #", (size_t)vm->vars[_i].any()); }
    if (vm->trace == TraceMode::TAIL) sd += "\n"; else LOG_PROGRAM(sd);
}

#ifndef NDEBUG
    #define CHECKI(B) if (vm->trace != TraceMode::OFF) CVM_Trace(vm, sp, B);
    #define CHECK(N, A) CHECKI(cat(#N, cat_parens A))
    #define CHECKJ(N) CHECKI(#N)
#else
    #define CHECK(N, A)
    #define CHECKJ(N)
#endif

fun_base_t CVM_GetNextCallTarget(VM *vm) {
    return vm->next_call_target;
}

// Only here because in compiled code we don't know sizeof(Value) (!)
StackPtr CVM_Drop(StackPtr sp) { return --sp; }

#define F(N, A, USE, DEF) \
    StackPtr CVM_##N(VM *vm, StackPtr sp VM_COMMA_IF(A) VM_OP_ARGSN(A)) { \
        CHECK(N, (VM_OP_PASSN(A))); return U_##N(*vm, sp VM_COMMA_IF(A) VM_OP_PASSN(A)); }
LVALOPNAMES
#undef F
#define F(N, A, USE, DEF) \
    StackPtr CVM_##N(VM *vm, StackPtr sp VM_COMMA_IF(A) VM_OP_ARGSN(A)) { \
        CHECK(N, (VM_OP_PASSN(A))); return U_##N(*vm, sp VM_COMMA_IF(A) VM_OP_PASSN(A)); }
ILBASENAMES
#undef F
#define F(N, A, USE, DEF) \
    StackPtr CVM_##N(VM *vm, StackPtr sp VM_COMMA_IF(A) VM_OP_ARGSN(A), fun_base_t fcont) { \
        /*LOG_ERROR("INS: ", #N, A, ", ", (size_t)vm, ", ", (size_t)sp, ", ", _a, ", ", (size_t)fcont);*/ \
        CHECK(N, (VM_OP_PASSN(A))); return U_##N(*vm, sp, VM_OP_PASSN(A) VM_COMMA_IF(A) fcont); }
ILCALLNAMES
#undef F
#define F(N, A, USE, DEF) \
    StackPtr CVM_##N(VM *vm, StackPtr sp) { CHECKJ(N); return U_##N(*vm, sp); }
ILJUMPNAMES1
#undef F
#define F(N, A, USE, DEF) \
    StackPtr CVM_##N(VM *vm, StackPtr sp, int df) { CHECKJ(N); return U_##N(*vm, sp, df); }
ILJUMPNAMES2
#undef F

#if VM_JIT_MODE

#if LOBSTER_ENGINE
extern "C" StackPtr GLFrame(StackPtr sp, VM & vm);
#endif

const void *vm_ops_jit_table[] = {
    #define F(N, A, USE, DEF) "U_" #N, (void *)&CVM_##N,
        ILNAMES
    #undef F
    "GetNextCallTarget", (void *)CVM_GetNextCallTarget,
    "Drop", (void *)CVM_Drop,
    #if LOBSTER_ENGINE
    "GLFrame", (void *)GLFrame,
    #endif
    0, 0
};
#endif

}  // extern "C"

