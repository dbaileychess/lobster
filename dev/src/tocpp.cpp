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
#include "lobster/disasm.h"  // Some shared bytecode utilities.
#include "lobster/compiler.h"
#include "lobster/tonative.h"

namespace lobster {

string ToCPP(NativeRegistry &natreg, string &sd, string_view bytecode_buffer, bool cpp) {
    auto bcf = bytecode::GetBytecodeFile(bytecode_buffer.data());
    if (!FLATBUFFERS_LITTLEENDIAN) return "native code gen requires little endian";
    auto code = (const int *)bcf->bytecode()->Data();  // Assumes we're on a little-endian machine.
    auto typetable = (const type_elem_t *)bcf->typetable()->Data();  // Same.
    auto function_lookup = CreateFunctionLookUp(bcf);

    if (cpp) {
        sd +=
            "#include \"lobster/stdafx.h\"\n"
            "#include \"lobster/vmdata.h\"\n"
            "#include \"lobster/vmops.h\"\n"
            "#include \"lobster/compiler.h\"\n"
            "\n"
            "typedef lobster::Value Value;\n"
            "typedef lobster::StackPtr StackPtr;\n"
            "typedef lobster::VM &VMRef;\n"
            "typedef lobster::fun_base_t fun_base_t;\n"
            "\n"
            "#if LOBSTER_ENGINE\n"
            "    // FIXME: This makes SDL not modular, but without it it will miss the SDLMain indirection.\n"
            "    #include \"lobster/sdlincludes.h\"\n"
            "    #include \"lobster/sdlinterface.h\"\n"
            "    extern \"C\" StackPtr GLFrame(StackPtr sp, VMRef vm);\n"
            "#endif\n"
            "\n"
            ;
    } else {
        sd +=
            // This needs to correspond to the C++ Value, enforced in Entry().
            "typedef struct {\n"
            "    union {\n"
            "        long long ival;\n"
            "        double fval;\n"
            "        void *rval;\n"
            "    };\n"
            #if RTT_ENABLED
            "    int type;\n"
            #endif
            "} Value;\n"
            "typedef Value *StackPtr;\n"
            "typedef void *VMRef;\n"
            "typedef StackPtr(*fun_base_t)(VMRef, StackPtr);\n"
            "#define Pop(sp) (*(sp)--)\n"
            "#define Push(sp, V) (*++(sp) = (V))\n"
            "#define TopM(sp, N) (*((sp) - (N)))\n"
            "\n"
            ;

        auto args = [&](int A) {
            for (int i = 0; i < A; i++) sd += ", int";
        };

        #define F(N, A, USE, DEF) \
            sd += "StackPtr U_" #N "(VMRef, StackPtr"; args(A); sd += ");\n";
            ILBASENAMES
        #undef F
        #define F(N, A, USE, DEF) \
            sd += "StackPtr U_" #N "(VMRef, StackPtr"; args(A); sd += ", fun_base_t);\n";
            ILCALLNAMES
        #undef F
        #define F(N, A, USE, DEF) \
            sd += "StackPtr U_" #N "(VMRef, StackPtr, const int *);\n";
            ILVARARGNAMES
        #undef F
        #define F(N, A, USE, DEF) \
            sd += "StackPtr U_" #N "(VMRef, StackPtr);\n";
            ILJUMPNAMES1
        #undef F
        #define F(N, A, USE, DEF) \
            sd += "StackPtr U_" #N "(VMRef, StackPtr, int);\n";
            ILJUMPNAMES2
        #undef F

        sd += "extern fun_base_t GetNextCallTarget(VMRef);\n"
              "extern void Entry(int);\n"
              "extern StackPtr GLFrame(StackPtr, VMRef);\n"
              "extern void SwapVars(VMRef, int, StackPtr, int);\n"
              "extern void BackupVar(VMRef, int, Value *);\n"
              "extern void NilVal(Value *);\n"
              "extern void DecOwned(VMRef, int);\n"
              "extern void DecVal(VMRef, Value);\n"
              "extern void RestoreBackup(VMRef, int, Value);\n"
              "extern StackPtr PopArg(VMRef, int, StackPtr);\n"
              "\n";
    }

    auto len = bcf->bytecode()->Length();
    auto ip = code;
    // Skip past 1st jump.
    assert(*ip == IL_JUMP);
    ip += 2;
    auto starting_ip = code + *ip++;
    int starting_point = -1;
    while (ip < code + len) {
        int id = (int)(ip - code);
        if (*ip == IL_FUNSTART || ip == starting_ip) {
            append(sd, "static StackPtr fun_", id, "(VMRef, StackPtr);\n");
            starting_point = id;
        }
        if ((false)) {  // Debug corrupt bytecode.
            string da;
            DisAsmIns(natreg, da, ip, code, typetable, bcf, true);
            LOG_DEBUG(da);
        }
        int opc = *ip++;
        if (opc < 0 || opc >= IL_MAX_OPS) {
            return cat("Corrupt bytecode: ", opc, " at: ", id);
        }
        int regso = -1;
        ParseOpAndGetArity(opc, ip, regso);
    }
    sd += "\n";
    vector<const int *> jumptables;
    ip = code + 3;  // Past first IL_JUMP.
    const int *funstart = nullptr;
    int nkeepvars = 0;
    int ndefsave = 0;
    string sdt;
    while (ip < code + len) {
        int id = (int)(ip - code);
        bool is_start = ip == starting_ip;
        int opc = *ip++;
        auto args = ip + 1;
        if (opc == IL_FUNSTART || is_start) {
            funstart = args;
            nkeepvars = 0;
            ndefsave = 0;
            sdt.clear();
            auto it = function_lookup.find(id);
            auto f = it != function_lookup.end() ? it->second : nullptr;
            sd += "\n";
            if (f) append(sd, "// ", f->name()->string_view(), "\n");
            append(sd, "static StackPtr fun_", id, "(VMRef vm, StackPtr psp) {\n");
            if (opc == IL_FUNSTART) {
                auto fip = funstart;
                fip++;  // definedfunction
                auto regs_max = *fip++;
                auto nargs_fun = *fip++;
                fip += nargs_fun;
                ndefsave = *fip++;
                fip += ndefsave;
                nkeepvars = *fip++;
                // FIXME: don't emit array.
                append(sd, "    Value regs[", max(1, regs_max), "];\n");
                if (ndefsave) append(sd, "    Value defsave[", ndefsave, "];\n");
                if (nkeepvars) append(sd, "    Value keepvar[", nkeepvars, "];\n");
            } else {
                // Final program return at most 1 value.
                append(sd, "    Value regs[1];\n");
            }
            append(sd, "    StackPtr sp = &regs[-1];\n");
        }
        int regso = -1;
        auto arity = ParseOpAndGetArity(opc, ip, regso);
        append(sd, "    ");
        if (cpp && opc != IL_SAVERETS && opc != IL_JUMPIFUNWOUND && opc != IL_RETURNANY && opc != IL_FUNSTART)   // FIXME
            append(sd, "assert(sp == &regs[", regso - 1, "]); ");
        if (opc == IL_FUNSTART) {
            auto fip = funstart;
            fip++;  // definedfunction
            fip++;  // regs_max.
            auto nargs_fun = *fip++;
            for (int i = 0; i < nargs_fun; i++) {
                append(sd, "\n    SwapVars(vm, ", fip[i], ", psp, ", nargs_fun - i - 1, ");");
            }
            fip += nargs_fun;
            ndefsave = *fip++;
            for (int i = 0; i < ndefsave; i++) {
                // for most locals, this just saves an nil, only in recursive cases it has an actual
                // value.
                auto varidx = *fip++;
                if (cpp) append(sd, "\n    defsave[", i, "] = BackupVar(vm, ", varidx, ");");
                else
                    append(sd, "\n    BackupVar(vm, ", varidx, ", &defsave[", i, "]);");
            }
            nkeepvars = *fip++;
            for (int i = 0; i < nkeepvars; i++) {
                if (cpp) append(sd, "\n    keepvar[", i, "] = lobster::NilVal();");  // FIXME ns
                else append(sd, "\n    NilVal(&keepvar[", i, "]);");
            }
        } else if (opc == IL_JUMP) {
            append(sd, "goto block", args[0], ";");
        } else if (CONDJUMP(opc)) {
            auto id = args[opc == IL_JUMPIFUNWOUND ? 1 : 0];
            assert(id >= 0);
            auto df = opc == IL_JUMPIFUNWOUND ? args[0] : -1;
            append(sd, "sp = U_", ILNames()[opc], "(vm, sp");
            if (df >= 0) append(sd, ", ", df);
            if (cpp) {
                append(sd, "); if (Pop(sp).False()) ");
                append(sd, "goto block", id, ";");
            } else {
                append(sd, "); { long long top = sp->ival; sp--; if (!top) ");
                append(sd, "goto block", id, ";");
                sd += " }";
            }
        } else if (opc == IL_BLOCK_START) {
            // FIXME: added ";" because blocks may end up just before "}" at the end of a
            // switch, and generate warnings/errors. Ideally not generate this block at all.
            append(sd, "block", id, ":;");
        } else if (opc == IL_JUMP_TABLE) {
            if (cpp) {
                sd += "switch (Pop(sp).ival()) {";
            } else {
                append(sd, "{ long long top = sp->ival; sp--; switch (top) {");
            }
            jumptables.push_back(args);
        } else if (opc == IL_JUMP_TABLE_CASE_START) {
            auto t = jumptables.back();
            auto mini = *t++;
            auto maxi = *t++;
            for (auto i = mini; i <= maxi; i++) {
                if (*t++ == id) {
                    append(sd, "case ", i, ":");
                }
            }
            if (*t++ == id) {
                append(sd, "default:");
            }
        } else if (opc == IL_JUMP_TABLE_END) {
            if (cpp) sd += "} // switch";
            else sd += "}} // switch";
            jumptables.pop_back();
        } else if (ISBCALL(opc) && natreg.nfuns[args[0]]->IsGLFrame()) {
            append(sd, "sp = GLFrame(sp, vm);");
        } else if (opc == IL_RETURN || opc == IL_RETURNANY) {
            // FIXME: emit epilogue stuff only once at end of function.
            auto fip = funstart;
            fip++;  // function id.
            fip++;  // regs_max.
            auto nargs = *fip++;
            auto freevars = fip + nargs;
            fip += nargs;
            auto ndef = *fip++;
            auto defvars = fip;
            fip += ndef;
            fip++;  // nkeepvars, already parsed above
            int nrets;
            if (opc == IL_RETURN) {
                nrets = args[1];
                append(sd, "psp = U_RETURN(vm, psp, ", args[0], ", ", nrets, ");");
            } else {
                nrets = args[0];
                append(sd, "psp = U_RETURNANY(vm, psp, ", nrets, ");");
            }
            auto ownedvars = *fip++;
            for (int i = 0; i < ownedvars; i++) {
                append(sd, "\n    DecOwned(vm, ", *fip, ");");
                fip++;
            }
            while (nargs--) {
                auto i = *--freevars;
                append(sd, "\n    psp = PopArg(vm, ", i, ", psp);");
            }
            for (int i = 0; i < nrets; i++) {
                append(sd, "\n    Push(psp, TopM(sp, ", nrets - i - 1, "));");
            }
            if (nrets) append(sd, "\n    sp -= ", nrets, ";");
            sdt.clear();  // FIXME: remove
            for (int i = 0; i < ndef; i++) {
                auto varidx = defvars[i];
                append(sdt, "    RestoreBackup(vm, ", varidx, ", defsave[", i, "]);\n");
            }
            if (opc == IL_RETURN) {
                append(sd, "\n    goto epilogue;");
            }
        } else if (opc == IL_SAVERETS) {  // FIXME: remove
            append(sd, "\n    goto epilogue;");
        } else if (opc == IL_KEEPREF || opc == IL_KEEPREFLOOP) {
            if (opc == IL_KEEPREFLOOP) append(sd, "DecVal(vm, keepvar[", args[1], "]); ");
            append(sd, "keepvar[", args[1], "] = TopM(sp, ", args[0], ");");
        } else {
            assert(ILArity()[opc] != ILUNKNOWN);
            append(sd, "sp = U_", ILNames()[opc], "(vm, sp");
            for (int i = 0; i < arity; i++) {
                sd += ", ";
                append(sd, args[i]);
            }
            if (opc == IL_PUSHFUN) {
                sd += ", ";
                append(sd, "fun_", args[0]);
            }
            sd += ");";

            string comment;
            if (opc == IL_PUSHVAR || opc == IL_PUSHVARV || opc == IL_LVAL_VAR) {
                comment = IdName(bcf, args[0], typetable, false);
            } else if (opc == IL_PUSHSTR) {
                auto sv = bcf->stringtable()->Get(args[0])->string_view();
                sv = sv.substr(0, 50);
                EscapeAndQuote(sv, comment, true);
            } else if (opc == IL_CALL) {
                auto fs = code + args[0];
                assert(*fs == IL_FUNSTART);
                fs += 2;
                comment = bcf->functions()->Get(*fs)->name()->string_view();
            } else if (ISBCALL(opc)) {
                comment = natreg.nfuns[args[0]]->name;
            } else if (opc == IL_ISTYPE || opc == IL_NEWOBJECT || opc == IL_ST2S) {
                auto ti = ((TypeInfo *)(typetable + args[0]));
                if (IsUDT(ti->t)) comment = bcf->udts()->Get(ti->structidx)->name()->string_view();
            }
            if (!comment.empty()) append(sd, " /* ", comment, " */");

            if (opc == IL_CALL) {
                append(sd, " sp = fun_", args[0], "(vm, sp);");
            } else if (opc == IL_CALLV || opc == IL_DDCALL) {
                sd += " ";
                if (cpp) sd += "sp = vm.next_call_target(vm, sp);";
                else sd += "sp = GetNextCallTarget(vm)(vm, sp);";
            }
        }
        sd += "\n";
        if (ip == code + len || *ip == IL_FUNSTART || ip == starting_ip) {
            if (opc != IL_EXIT && opc != IL_ABORT) sd += "    epilogue:\n";
            if (!sdt.empty()) append(sd, sdt);
            for (int i = 0; i < nkeepvars; i++) {
                append(sd, "    DecVal(vm, keepvar[", i, "]);\n");
            }
            sd += "    return psp;\n";
            sd += "}\n";
        }
    }

    if (cpp) sd += "\nstatic";
    else sd += "\nextern ";
    sd += " const fun_base_t vtables[] = {\n";
    for (auto id : *bcf->vtables()) {
        sd += "    ";
        if (id >= 0) append(sd, "fun_", id);
        else sd += "0";
        sd += ",\n";
    }
    sd += "    0\n};\n";  // Make sure table is never empty.

    if (cpp) {
        // FIXME: this obviously does NOT need to include the actual bytecode, just the metadata.
        // in fact, it be nice if those were in readable format in the generated code.
        sd += "\nstatic const int bytecodefb[] = {";
        auto bytecode_ints = (const int *)bytecode_buffer.data();
        for (size_t i = 0; i < bytecode_buffer.length() / sizeof(int); i++) {
            if ((i & 0xF) == 0) sd += "\n ";
            append(sd, " ", bytecode_ints[i], ",");
        }
        sd += "\n};\n\n";
    }
    if (cpp) sd += "extern \"C\" ";
    sd += "StackPtr compiled_entry_point(VMRef vm, StackPtr sp) {\n";
    if (!cpp) sd += "    Entry(sizeof(Value));\n";
    append(sd, "    return fun_", starting_point, "(vm, sp);\n}\n\n");
    if (cpp) {
        sd += "int main(int argc, char *argv[]) {\n";
        sd += "    // This is hard-coded to call compiled_entry_point()\n";
        sd += "    return RunCompiledCodeMain(argc, argv, ";
        append(sd, "(uint8_t *)bytecodefb, ", bytecode_buffer.size(), ", vtables);\n}\n");
    }

    return "";
}

}  // namespace lobster

