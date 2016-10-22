/*
** thingdef_expression.cpp
**
** Expression evaluation
**
**---------------------------------------------------------------------------
** Copyright 2008 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
** 4. When not used as part of ZDoom or a ZDoom derivative, this code will be
**    covered by the terms of the GNU General Public License as published by
**    the Free Software Foundation; either version 2 of the License, or (at
**    your option) any later version.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include <stdlib.h>
#include "actor.h"
#include "sc_man.h"
#include "tarray.h"
#include "templates.h"
#include "cmdlib.h"
#include "i_system.h"
#include "m_random.h"
#include "a_pickups.h"
#include "thingdef.h"
#include "p_lnspec.h"
#include "doomstat.h"
#include "codegen.h"
#include "m_fixed.h"
#include "vmbuilder.h"
#include "v_text.h"
#include "math/cmath.h"

struct FLOP
{
	ENamedName Name;
	int Flop;
	double (*Evaluate)(double);
};

// Decorate operates on degrees, so the evaluate functions need to convert
// degrees to radians for those that work with angles.
static const FLOP FxFlops[] =
{
	{ NAME_Exp,		FLOP_EXP,		[](double v) { return g_exp(v); } },
	{ NAME_Log,		FLOP_LOG,		[](double v) { return g_log(v); } },
	{ NAME_Log10,	FLOP_LOG10,		[](double v) { return g_log10(v); } },
	{ NAME_Sqrt,	FLOP_SQRT,		[](double v) { return g_sqrt(v); } },
	{ NAME_Ceil,	FLOP_CEIL,		[](double v) { return ceil(v); } },
	{ NAME_Floor,	FLOP_FLOOR,		[](double v) { return floor(v); } },

	{ NAME_ACos,	FLOP_ACOS_DEG,	[](double v) { return g_acos(v) * (180.0 / M_PI); } },
	{ NAME_ASin,	FLOP_ASIN_DEG,	[](double v) { return g_asin(v) * (180.0 / M_PI); } },
	{ NAME_ATan,	FLOP_ATAN_DEG,	[](double v) { return g_atan(v) * (180.0 / M_PI); } },
	{ NAME_Cos,		FLOP_COS_DEG,	[](double v) { return g_cosdeg(v); } },
	{ NAME_Sin,		FLOP_SIN_DEG,	[](double v) { return g_sindeg(v); } },
	{ NAME_Tan,		FLOP_TAN_DEG,	[](double v) { return g_tan(v * (M_PI / 180.0)); } },

	{ NAME_CosH,	FLOP_COSH,		[](double v) { return g_cosh(v); } },
	{ NAME_SinH,	FLOP_SINH,		[](double v) { return g_sinh(v); } },
	{ NAME_TanH,	FLOP_TANH,		[](double v) { return g_tanh(v); } },
};

//==========================================================================
//
// FCompileContext
//
//==========================================================================

FCompileContext::FCompileContext(PFunction *fnc, PPrototype *ret, bool fromdecorate) : ReturnProto(ret), Function(fnc), Class(nullptr), FromDecorate(fromdecorate)
{
	if (fnc != nullptr) Class = fnc->OwningClass;
}

FCompileContext::FCompileContext(PClass *cls) : ReturnProto(nullptr), Function(nullptr), Class(cls), FromDecorate(true)	// only used by DECORATE constants.
{
}

PSymbol *FCompileContext::FindInClass(FName identifier, PSymbolTable *&symt)
{
	return Class != nullptr? Class->Symbols.FindSymbolInTable(identifier, symt) : nullptr;
}

PSymbol *FCompileContext::FindInSelfClass(FName identifier, PSymbolTable *&symt)
{
	// If we have no self we cannot retrieve any values from it.
	if (Function == nullptr || Function->Variants[0].SelfClass == nullptr) return nullptr;
	return Function->Variants[0].SelfClass->Symbols.FindSymbolInTable(identifier, symt);
}
PSymbol *FCompileContext::FindGlobal(FName identifier)
{
	return GlobalSymbols.FindSymbol(identifier, true);
}

void FCompileContext::CheckReturn(PPrototype *proto, FScriptPosition &pos)
{
	assert(proto != nullptr);
	bool fail = false;

	if (ReturnProto == nullptr)
	{
		ReturnProto = proto;
		return;
	}

	// A prototype that defines fewer return types can be compatible with
	// one that defines more if the shorter one matches the initial types
	// for the longer one.
	if (ReturnProto->ReturnTypes.Size() < proto->ReturnTypes.Size())
	{ // Make proto the shorter one to avoid code duplication below.
		swapvalues(proto, ReturnProto);
	}
	// If one prototype returns nothing, they both must.
	if (proto->ReturnTypes.Size() == 0)
	{
		if (ReturnProto->ReturnTypes.Size() != 0)
		{
			fail = true;
		}
	}
	else
	{
		for (unsigned i = 0; i < proto->ReturnTypes.Size(); i++)
		{
			if (ReturnProto->ReturnTypes[i] != proto->ReturnTypes[i])
			{ // Incompatible
				fail = true;
				break;
			}
		}
	}

	if (fail)
	{
		pos.Message(MSG_ERROR, "All return expressions must deduce to the same type");
	}
}

FxLocalVariableDeclaration *FCompileContext::FindLocalVariable(FName name)
{
	if (Block == nullptr)
	{
		return nullptr;
	}
	else
	{
		return Block->FindLocalVariable(name, *this);
	}
}

//==========================================================================
//
// ExpEmit
//
//==========================================================================

ExpEmit::ExpEmit(VMFunctionBuilder *build, int type)
: RegNum(build->Registers[type].Get(1)), RegType(type), Konst(false), Fixed(false), Final(false), Target(false)
{
}

void ExpEmit::Free(VMFunctionBuilder *build)
{
	if (!Fixed && !Konst && RegType <= REGT_TYPE)
	{
		build->Registers[RegType].Return(RegNum, 1);
	}
}

void ExpEmit::Reuse(VMFunctionBuilder *build)
{
	if (!Fixed && !Konst)
	{
		bool success = build->Registers[RegType].Reuse(RegNum);
		assert(success && "Attempt to reuse a register that is already in use");
	}
}

//==========================================================================
//
// FindDecorateBuiltinFunction
//
// Returns the symbol for a decorate utility function. If not found, create
// it and install it in Actor.
//
//==========================================================================

static PSymbol *FindDecorateBuiltinFunction(FName funcname, VMNativeFunction::NativeCallType func)
{
	PSymbol *sym = RUNTIME_CLASS(AActor)->Symbols.FindSymbol(funcname, false);
	if (sym == NULL)
	{
		PSymbolVMFunction *symfunc = new PSymbolVMFunction(funcname);
		VMNativeFunction *calldec = new VMNativeFunction(func, funcname);
		symfunc->Function = calldec;
		sym = symfunc;
		RUNTIME_CLASS(AActor)->Symbols.AddSymbol(sym);
	}
	return sym;
}

//==========================================================================
//
//
//
//==========================================================================

static bool AreCompatiblePointerTypes(PType *dest, PType *source)
{
	if (dest->IsKindOf(RUNTIME_CLASS(PPointer)) && source->IsKindOf(RUNTIME_CLASS(PPointer)))
	{
		// Pointers to different types are only compatible if both point to an object and the source type is a child of the destination type.
		auto fromtype = static_cast<PPointer *>(source);
		auto totype = static_cast<PPointer *>(dest);
		if (fromtype == nullptr) return true;
		if (fromtype == totype) return true;
		if (fromtype->PointedType->IsKindOf(RUNTIME_CLASS(PClass)) && totype->PointedType->IsKindOf(RUNTIME_CLASS(PClass)))
		{
			auto fromcls = static_cast<PClass *>(fromtype->PointedType);
			auto tocls = static_cast<PClass *>(totype->PointedType);
			return (fromcls->IsDescendantOf(tocls));
		}
	}
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxExpression::Emit (VMFunctionBuilder *build)
{
	ScriptPosition.Message(MSG_ERROR, "Unemitted expression found");
	return ExpEmit();
}


//==========================================================================
//
//
//
//==========================================================================

bool FxExpression::isConstant() const
{
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

VMFunction *FxExpression::GetDirectFunction()
{
	return NULL;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxExpression::Resolve(FCompileContext &ctx)
{
	isresolved = true;
	return this;
}

//==========================================================================
//
// Returns true if we can write to the address.
//
//==========================================================================

bool FxExpression::RequestAddress(bool *writable)
{
	if (writable != nullptr) *writable = false;
	return false;
}

//==========================================================================
//
// Called by return statements.
//
//==========================================================================

PPrototype *FxExpression::ReturnProto()
{
	assert(ValueType != nullptr);

	TArray<PType *> ret(0);
	TArray<PType *> none(0);
	if (ValueType != TypeVoid)
	{
		ret.Push(ValueType);
	}

	return NewPrototype(ret, none);
}

//==========================================================================
//
//
//
//==========================================================================

static void EmitParameter(VMFunctionBuilder *build, FxExpression *operand, const FScriptPosition &pos)
{
	ExpEmit where = operand->Emit(build);

	if (where.RegType == REGT_NIL)
	{
		pos.Message(MSG_ERROR, "Attempted to pass a non-value");
		build->Emit(OP_PARAM, 0, where.RegType, where.RegNum);
	}
	else
	{
		int regtype = where.RegType;
		if (where.Konst)
		{
			regtype |= REGT_KONST;
		}
		build->Emit(OP_PARAM, 0, regtype, where.RegNum);
		where.Free(build);
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxConstant::MakeConstant(PSymbol *sym, const FScriptPosition &pos)
{
	FxExpression *x;
	PSymbolConstNumeric *csym = dyn_cast<PSymbolConstNumeric>(sym);
	if (csym != NULL)
	{
		if (csym->ValueType->IsA(RUNTIME_CLASS(PInt)))
		{
			x = new FxConstant(csym->Value, pos);
		}
		else if (csym->ValueType->IsA(RUNTIME_CLASS(PFloat)))
		{
			x = new FxConstant(csym->Float, pos);
		}
		else
		{
			pos.Message(MSG_ERROR, "Invalid constant '%s'\n", csym->SymbolName.GetChars());
			return NULL;
		}
	}
	else
	{
		pos.Message(MSG_ERROR, "'%s' is not a constant\n", sym->SymbolName.GetChars());
		x = NULL;
	}
	return x;
}

ExpEmit FxConstant::Emit(VMFunctionBuilder *build)
{
	ExpEmit out;

	out.Konst = true;
	int regtype = value.Type->GetRegType();
	out.RegType = regtype;
	if (regtype == REGT_INT)
	{
		out.RegNum = build->GetConstantInt(value.Int);
	}
	else if (regtype == REGT_FLOAT)
	{
		out.RegNum = build->GetConstantFloat(value.Float);
	}
	else if (regtype == REGT_POINTER)
	{
		VM_ATAG tag = ATAG_GENERIC;
		if (value.Type == TypeState)
		{
			tag = ATAG_STATE;
		}
		else if (value.Type->GetLoadOp() == OP_LO)
		{
			tag = ATAG_OBJECT;
		}
		out.RegNum = build->GetConstantAddress(value.pointer, tag);
	}
	else if (regtype == REGT_STRING)
	{
		out.RegNum = build->GetConstantString(value.GetString());
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot emit needed constant");
		out.RegNum = 0;
	}
	return out;
}

//==========================================================================
//
//
//
//==========================================================================

FxBoolCast::FxBoolCast(FxExpression *x)
	: FxExpression(x->ScriptPosition)
{
	basex = x;
	ValueType = TypeBool;
}

//==========================================================================
//
//
//
//==========================================================================

FxBoolCast::~FxBoolCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxBoolCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeBool)
	{
		FxExpression *x = basex;
		basex = nullptr;
		delete this;
		return x;
	}
	else if (basex->ValueType->GetRegType() == REGT_INT || basex->ValueType->GetRegType() == REGT_FLOAT || basex->ValueType->GetRegType() == REGT_POINTER)
	{
		if (basex->isConstant())
		{
			assert(basex->ValueType != TypeState && "We shouldn't be able to generate a constant state ref");

			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(constval.GetBool(), ScriptPosition);
			delete this;
			return x;
		}
		return this;
	}
	ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
	delete this;
	return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxBoolCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType->GetRegType() == REGT_INT || basex->ValueType->GetRegType() == REGT_FLOAT || basex->ValueType->GetRegType() == REGT_POINTER);
	ExpEmit to(build, REGT_INT);
	from.Free(build);

	// Preload result with 0.
	build->Emit(OP_LI, to.RegNum, 0);

	// Check source against 0.
	if (from.RegType == REGT_INT)
	{
		build->Emit(OP_EQ_R, 1, from.RegNum, to.RegNum);
	}
	else if (from.RegType == REGT_FLOAT)
	{
		build->Emit(OP_EQF_K, 1, from.RegNum, build->GetConstantFloat(0.));
	}
	else if (from.RegType == REGT_POINTER)
	{
		build->Emit(OP_EQA_K, 1, from.RegNum, build->GetConstantAddress(nullptr, ATAG_GENERIC));
	}
	build->Emit(OP_JMP, 1);

	// Reload result with 1 if the comparison fell through.
	build->Emit(OP_LI, to.RegNum, 1);

	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxIntCast::FxIntCast(FxExpression *x, bool nowarn)
: FxExpression(x->ScriptPosition)
{
	basex=x;
	ValueType = TypeSInt32;
	NoWarn = nowarn;
}

//==========================================================================
//
//
//
//==========================================================================

FxIntCast::~FxIntCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxIntCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType->GetRegType() == REGT_INT)
	{
		if (basex->ValueType != TypeName)
		{
			FxExpression *x = basex;
			basex = NULL;
			delete this;
			return x;
		}
		else
		{
			// Ugh. This should abort, but too many mods fell into this logic hole somewhere, so this seroious error needs to be reduced to a warning. :(
			// At least in ZScript, MSG_OPTERROR always means to report an error, not a warning so the problem only exists in DECORATE.
			if (!basex->isConstant())	ScriptPosition.Message(MSG_OPTERROR, "Numeric type expected, got a name");
			else ScriptPosition.Message(MSG_OPTERROR, "Numeric type expected, got \"%s\"", static_cast<FxConstant*>(basex)->GetValue().GetName().GetChars());
			FxExpression * x = new FxConstant(0, ScriptPosition);
			delete this;
			return x;
		}
	}
	else if (basex->ValueType->GetRegType() == REGT_FLOAT)
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(constval.GetInt(), ScriptPosition);
			if (!NoWarn && constval.GetInt() != constval.GetFloat())
			{
				ScriptPosition.Message(MSG_WARNING, "Truncation of floating point constant %f", constval.GetFloat());
			}

			delete this;
			return x;
		}
		else if (!NoWarn)
		{
			ScriptPosition.Message(MSG_WARNING, "Truncation of floating point value");
		}

		return this;
	}
	ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
	delete this;
	return NULL;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxIntCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType->GetRegType() == REGT_FLOAT);
	from.Free(build);
	ExpEmit to(build, REGT_INT);
	build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_F2I);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxFloatCast::FxFloatCast(FxExpression *x)
	: FxExpression(x->ScriptPosition)
{
	basex = x;
	ValueType = TypeFloat64;
}

//==========================================================================
//
//
//
//==========================================================================

FxFloatCast::~FxFloatCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxFloatCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType->GetRegType() == REGT_FLOAT)
	{
		FxExpression *x = basex;
		basex = NULL;
		delete this;
		return x;
	}
	else if (basex->ValueType->GetRegType() == REGT_INT)
	{
		if (basex->ValueType != TypeName)
		{
			if (basex->isConstant())
			{
				ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
				FxExpression *x = new FxConstant(constval.GetFloat(), ScriptPosition);
				delete this;
				return x;
			}
			return this;
		}
		else
		{
			// Ugh. This should abort, but too many mods fell into this logic hole somewhere, so this seroious error needs to be reduced to a warning. :(
			// At least in ZScript, MSG_OPTERROR always means to report an error, not a warning so the problem only exists in DECORATE.
			if (!basex->isConstant()) ScriptPosition.Message(MSG_OPTERROR, "Numeric type expected, got a name");
			else ScriptPosition.Message(MSG_OPTERROR, "Numeric type expected, got \"%s\"", static_cast<FxConstant*>(basex)->GetValue().GetName().GetChars());
			FxExpression *x = new FxConstant(0.0, ScriptPosition);
			delete this;
			return x;
		}
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return NULL;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxFloatCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType->GetRegType() == REGT_INT);
	from.Free(build);
	ExpEmit to(build, REGT_FLOAT);
	build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_I2F);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxNameCast::FxNameCast(FxExpression *x)
	: FxExpression(x->ScriptPosition)
{
	basex = x;
	ValueType = TypeName;
}

//==========================================================================
//
//
//
//==========================================================================

FxNameCast::~FxNameCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxNameCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeName)
	{
		FxExpression *x = basex;
		basex = NULL;
		delete this;
		return x;
	}
	else if (basex->ValueType == TypeString)
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(constval.GetName(), ScriptPosition);
			delete this;
			return x;
		}
		return this;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot convert to name");
		delete this;
		return NULL;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxNameCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType == TypeString);
	from.Free(build);
	ExpEmit to(build, REGT_INT);
	build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_S2N);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxStringCast::FxStringCast(FxExpression *x)
	: FxExpression(x->ScriptPosition)
{
	basex = x;
	ValueType = TypeString;
}

//==========================================================================
//
//
//
//==========================================================================

FxStringCast::~FxStringCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxStringCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeString)
	{
		FxExpression *x = basex;
		basex = NULL;
		delete this;
		return x;
	}
	else if (basex->ValueType == TypeName)
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(constval.GetString(), ScriptPosition);
			delete this;
			return x;
		}
		return this;
	}
	else if (basex->ValueType == TypeSound)
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(S_sfx[constval.GetInt()].name, ScriptPosition);
			delete this;
			return x;
		}
		return this;
	}
	// although it could be done, let's not convert colors back to strings.
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot convert to string");
		delete this;
		return NULL;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxStringCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);

	from.Free(build);
	ExpEmit to(build, REGT_STRING);
	if (ValueType == TypeName)
	{
		build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_N2S);
	}
	else if (ValueType == TypeSound)
	{
		build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_So2S);
	}
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxColorCast::FxColorCast(FxExpression *x)
	: FxExpression(x->ScriptPosition)
{
	basex = x;
	ValueType = TypeColor;
}

//==========================================================================
//
//
//
//==========================================================================

FxColorCast::~FxColorCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxColorCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeColor || basex->ValueType->GetClass() == RUNTIME_CLASS(PInt))
	{
		FxExpression *x = basex;
		x->ValueType = TypeColor;
		basex = NULL;
		delete this;
		return x;
	}
	else if (basex->ValueType == TypeString)
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(V_GetColor(nullptr, constval.GetString()), ScriptPosition);
			delete this;
			return x;
		}
		return this;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot convert to color");
		delete this;
		return NULL;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxColorCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType == TypeString);
	from.Free(build);
	ExpEmit to(build, REGT_INT);
	build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_S2Co);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxSoundCast::FxSoundCast(FxExpression *x)
	: FxExpression(x->ScriptPosition)
{
	basex = x;
	ValueType = TypeSound;
}

//==========================================================================
//
//
//
//==========================================================================

FxSoundCast::~FxSoundCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxSoundCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeSound || basex->ValueType->GetClass() == RUNTIME_CLASS(PInt))
	{
		FxExpression *x = basex;
		x->ValueType = TypeSound;
		basex = NULL;
		delete this;
		return x;
	}
	else if (basex->ValueType == TypeString)
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(FSoundID(constval.GetString()), ScriptPosition);
			delete this;
			return x;
		}
		return this;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot convert to sound");
		delete this;
		return NULL;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxSoundCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType == TypeString);
	from.Free(build);
	ExpEmit to(build, REGT_INT);
	build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_S2So);
	return to;
}

//==========================================================================
//
// generic type cast operator
//
//==========================================================================

FxTypeCast::FxTypeCast(FxExpression *x, PType *type, bool nowarn)
	: FxExpression(x->ScriptPosition)
{
	basex = x;
	ValueType = type;
}

//==========================================================================
//
//
//
//==========================================================================

FxTypeCast::~FxTypeCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxTypeCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	// first deal with the simple types
	if (ValueType == TypeError || basex->ValueType == TypeError)
	{
		delete this;
		return nullptr;
	}
	else if (ValueType == TypeVoid)	// this should never happen
	{
		goto errormsg;
	}
	else if (basex->ValueType == TypeVoid)
	{
		goto errormsg;
	}
	else if (basex->ValueType == ValueType)
	{
		// don't go through the entire list if the types are the same.
		goto basereturn;
	}
	else if (basex->ValueType == TypeNullPtr && (ValueType == TypeState || ValueType->IsKindOf(RUNTIME_CLASS(PPointer))))
	{
		goto basereturn;
	}
	else if (ValueType->GetRegType() == REGT_FLOAT)
	{
		FxExpression *x = new FxFloatCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType->IsA(RUNTIME_CLASS(PInt)))
	{
		// This is only for casting to actual ints. Subtypes representing an int will be handled elsewhere.
		FxExpression *x = new FxIntCast(basex, NoWarn);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeBool)
	{
		FxExpression *x = new FxBoolCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeString)
	{
		FxExpression *x = new FxStringCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeName)
	{
		FxExpression *x = new FxNameCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeSound)
	{
		FxExpression *x = new FxSoundCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeColor)
	{
		FxExpression *x = new FxColorCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeState)
	{
		// Right now this only supports string constants. There should be an option to pass a string variable, too.
		if (basex->isConstant() && (basex->ValueType == TypeString || basex->ValueType == TypeName))
		{
			FxExpression *x = new FxMultiNameState(static_cast<FxConstant *>(basex)->GetValue().GetString(), basex->ScriptPosition);
			x = x->Resolve(ctx);
			basex = nullptr;
			delete this;
			return x;
		}
	}
	else if (ValueType->IsKindOf(RUNTIME_CLASS(PClassPointer)))
	{
		FxExpression *x = new FxClassTypeCast(static_cast<PClassPointer*>(ValueType), basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	/* else if (ValueType->IsKindOf(RUNTIME_CLASS(PEnum)))
	{
	// this is not yet ready and does not get assigned to actual values.
	}
	*/
	else if (ValueType->IsKindOf(RUNTIME_CLASS(PClass)))	// this should never happen because the VM doesn't handle plain class types - just pointers
	{
		if (basex->ValueType->IsKindOf(RUNTIME_CLASS(PClass)))
		{
			// class types are only compatible if the base type is a descendant of the result type.
			auto fromtype = static_cast<PClass *>(basex->ValueType);
			auto totype = static_cast<PClass *>(ValueType);
			if (fromtype->IsDescendantOf(totype)) goto basereturn;
		}
	}
	else if (AreCompatiblePointerTypes(ValueType, basex->ValueType))
	{
		goto basereturn;
	}
	// todo: pointers to class objects. 
	// All other types are only compatible to themselves and have already been handled above by the equality check.
	// Anything that falls through here is not compatible and must print an error.

errormsg:
	ScriptPosition.Message(MSG_ERROR, "Cannot convert %s to %s", basex->ValueType->DescriptiveName(), ValueType->DescriptiveName());
	delete this;
	return nullptr;

basereturn:
	auto x = basex;
	x->ValueType = ValueType;
	basex = nullptr;
	delete this;
	return x;

}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxTypeCast::Emit(VMFunctionBuilder *build)
{
	assert(false);
	// This should never be reached
	return ExpEmit();
}

//==========================================================================
//
//
//
//==========================================================================

FxPlusSign::FxPlusSign(FxExpression *operand)
: FxExpression(operand->ScriptPosition)
{
	Operand=operand;
}

//==========================================================================
//
//
//
//==========================================================================

FxPlusSign::~FxPlusSign()
{
	SAFE_DELETE(Operand);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxPlusSign::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Operand, ctx);

	if (Operand->IsNumeric())
	{
		FxExpression *e = Operand;
		Operand = NULL;
		delete this;
		return e;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return NULL;
	}
}

ExpEmit FxPlusSign::Emit(VMFunctionBuilder *build)
{
	return Operand->Emit(build);
}

//==========================================================================
//
//
//
//==========================================================================

FxMinusSign::FxMinusSign(FxExpression *operand)
: FxExpression(operand->ScriptPosition)
{
	Operand=operand;
}

//==========================================================================
//
//
//
//==========================================================================

FxMinusSign::~FxMinusSign()
{
	SAFE_DELETE(Operand);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxMinusSign::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Operand, ctx);

	if (Operand->IsNumeric())
	{
		if (Operand->isConstant())
		{
			ExpVal val = static_cast<FxConstant *>(Operand)->GetValue();
			FxExpression *e = val.Type->GetRegType() == REGT_INT ?
				new FxConstant(-val.Int, ScriptPosition) :
				new FxConstant(-val.Float, ScriptPosition);
			delete this;
			return e;
		}
		ValueType = Operand->ValueType;
		return this;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return NULL;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxMinusSign::Emit(VMFunctionBuilder *build)
{
	assert(ValueType == Operand->ValueType);
	ExpEmit from = Operand->Emit(build);
	assert(from.Konst == 0);
	// Do it in-place.
	if (ValueType->GetRegType() == REGT_INT)
	{
		build->Emit(OP_NEG, from.RegNum, from.RegNum, 0);
	}
	else
	{
		assert(ValueType->GetRegType() == REGT_FLOAT);
		build->Emit(OP_FLOP, from.RegNum, from.RegNum, FLOP_NEG);
	}
	return from;
}

//==========================================================================
//
//
//
//==========================================================================

FxUnaryNotBitwise::FxUnaryNotBitwise(FxExpression *operand)
: FxExpression(operand->ScriptPosition)
{
	Operand=operand;
}

//==========================================================================
//
//
//
//==========================================================================

FxUnaryNotBitwise::~FxUnaryNotBitwise()
{
	SAFE_DELETE(Operand);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxUnaryNotBitwise::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Operand, ctx);

	if  (Operand->ValueType->GetRegType() == REGT_FLOAT /* lax */)
	{
		// DECORATE allows floats here so cast them to int.
		Operand = new FxIntCast(Operand, ctx.FromDecorate);
		Operand = Operand->Resolve(ctx);
		if (Operand == NULL) 
		{
			delete this;
			return NULL;
		}
	}

	if (Operand->ValueType->GetRegType() != REGT_INT)
	{
		ScriptPosition.Message(MSG_ERROR, "Integer type expected");
		delete this;
		return NULL;
	}

	if (Operand->isConstant())
	{
		int result = ~static_cast<FxConstant *>(Operand)->GetValue().GetInt();
		FxExpression *e = new FxConstant(result, ScriptPosition);
		delete this;
		return e;
	}
	ValueType = TypeSInt32;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxUnaryNotBitwise::Emit(VMFunctionBuilder *build)
{
	assert(Operand->ValueType->GetRegType() == REGT_INT);
	ExpEmit from = Operand->Emit(build);
	assert(!from.Konst);
	// Do it in-place.
	build->Emit(OP_NOT, from.RegNum, from.RegNum, 0);
	return from;
}

//==========================================================================
//
//
//
//==========================================================================

FxUnaryNotBoolean::FxUnaryNotBoolean(FxExpression *operand)
: FxExpression(operand->ScriptPosition)
{
	Operand=operand;
}

//==========================================================================
//
//
//
//==========================================================================

FxUnaryNotBoolean::~FxUnaryNotBoolean()
{
	SAFE_DELETE(Operand);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxUnaryNotBoolean::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Operand, ctx);

	if (Operand->ValueType != TypeBool)
	{
		Operand = new FxBoolCast(Operand);
		SAFE_RESOLVE(Operand, ctx);
	}

	if (Operand->isConstant())
	{
		bool result = !static_cast<FxConstant *>(Operand)->GetValue().GetBool();
		FxExpression *e = new FxConstant(result, ScriptPosition);
		delete this;
		return e;
	}

	ValueType = TypeBool;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxUnaryNotBoolean::Emit(VMFunctionBuilder *build)
{
	assert(Operand->ValueType == ValueType);
	assert(ValueType == TypeBool);
	ExpEmit from = Operand->Emit(build);
	assert(!from.Konst);
	// ~x & 1
	build->Emit(OP_NOT, from.RegNum, from.RegNum, 0);
	build->Emit(OP_AND_RK, from.RegNum, from.RegNum, build->GetConstantInt(1));
	return from;
}

//==========================================================================
//
//
//
//==========================================================================

FxSizeAlign::FxSizeAlign(FxExpression *operand, int which)
	: FxExpression(operand->ScriptPosition)
{
	Operand = operand;
	Which = which;
}

//==========================================================================
//
//
//
//==========================================================================

FxSizeAlign::~FxSizeAlign()
{
	SAFE_DELETE(Operand);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxSizeAlign::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Operand, ctx);

	auto type = Operand->ValueType;
	if (Operand->isConstant())
	{
		ScriptPosition.Message(MSG_ERROR, "cannot determine %s of a constant", Which == 'a'? "alignment" : "size");
		delete this;
		return NULL;
	}
	else if (!Operand->RequestAddress(nullptr))
	{
		ScriptPosition.Message(MSG_ERROR, "Operand must be addressable to determine %s", Which == 'a' ? "alignment" : "size");
		delete this;
		return NULL;
	}
	else
	{
		FxExpression *x = new FxConstant(Which == TK_AlignOf ? int(type->Align) : int(type->Size), Operand->ScriptPosition);
		delete this;
		return x->Resolve(ctx);
	}
}

ExpEmit FxSizeAlign::Emit(VMFunctionBuilder *build)
{
	return ExpEmit();
}

//==========================================================================
//
// FxPreIncrDecr
//
//==========================================================================

FxPreIncrDecr::FxPreIncrDecr(FxExpression *base, int token)
: FxExpression(base->ScriptPosition), Token(token), Base(base)
{
	AddressRequested = false;
	AddressWritable = false;
}

FxPreIncrDecr::~FxPreIncrDecr()
{
	SAFE_DELETE(Base);
}

bool FxPreIncrDecr::RequestAddress(bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = AddressWritable;
	return true;
}

FxExpression *FxPreIncrDecr::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Base, ctx);

	ValueType = Base->ValueType;

	if (!Base->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return nullptr;
	}
	else if (Base->ValueType == TypeBool)
	{
		ScriptPosition.Message(MSG_ERROR, "%s is not allowed on type bool", FScanner::TokenName(Token).GetChars());
		delete this;
		return nullptr;
	}
	if (!Base->RequestAddress(&AddressWritable) || !AddressWritable )
	{
		ScriptPosition.Message(MSG_ERROR, "Expression must be a modifiable value");
		delete this;
		return nullptr;
	}

	return this;
}

ExpEmit FxPreIncrDecr::Emit(VMFunctionBuilder *build)
{
	assert(Token == TK_Incr || Token == TK_Decr);
	assert(ValueType == Base->ValueType && IsNumeric());

	int zero = build->GetConstantInt(0);
	int regtype = ValueType->GetRegType();
	ExpEmit pointer = Base->Emit(build);
	ExpEmit value = pointer;

	if (!pointer.Target)
	{
		value = ExpEmit(build, regtype);
		build->Emit(ValueType->GetLoadOp(), value.RegNum, pointer.RegNum, zero);
	}

	if (regtype == REGT_INT)
	{
		build->Emit((Token == TK_Incr) ? OP_ADD_RK : OP_SUB_RK, value.RegNum, value.RegNum, build->GetConstantInt(1));
	}
	else
	{
		build->Emit((Token == TK_Incr) ? OP_ADDF_RK : OP_SUBF_RK, value.RegNum, value.RegNum, build->GetConstantFloat(1.));
	}

	if (!pointer.Target)
	{
		build->Emit(ValueType->GetStoreOp(), pointer.RegNum, value.RegNum, zero);
	}

	if (AddressRequested)
	{
		value.Free(build);
		return pointer;
	}

	pointer.Free(build);
	return value;
}

//==========================================================================
//
// FxPostIncrDecr
//
//==========================================================================

FxPostIncrDecr::FxPostIncrDecr(FxExpression *base, int token)
: FxExpression(base->ScriptPosition), Token(token), Base(base)
{
}

FxPostIncrDecr::~FxPostIncrDecr()
{
	SAFE_DELETE(Base);
}

FxExpression *FxPostIncrDecr::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Base, ctx);
	bool AddressWritable;

	ValueType = Base->ValueType;

	if (!Base->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return nullptr;
	}
	else if (Base->ValueType == TypeBool)
	{
		ScriptPosition.Message(MSG_ERROR, "%s is not allowed on type bool", FScanner::TokenName(Token).GetChars());
		delete this;
		return nullptr;
	}
	if (!Base->RequestAddress(&AddressWritable) || !AddressWritable)
	{
		ScriptPosition.Message(MSG_ERROR, "Expression must be a modifiable value");
		delete this;
		return nullptr;
	}

	return this;
}

ExpEmit FxPostIncrDecr::Emit(VMFunctionBuilder *build)
{
	assert(Token == TK_Incr || Token == TK_Decr);
	assert(ValueType == Base->ValueType && IsNumeric());

	int zero = build->GetConstantInt(0);
	int regtype = ValueType->GetRegType();
	ExpEmit pointer = Base->Emit(build);
	ExpEmit out = pointer;

	if (!pointer.Target)
	{
		out = ExpEmit(build, regtype);
		build->Emit(ValueType->GetLoadOp(), out.RegNum, pointer.RegNum, zero);
	}

	ExpEmit assign(build, regtype);
	if (regtype == REGT_INT)
	{
		build->Emit((Token == TK_Incr) ? OP_ADD_RK : OP_SUB_RK, assign.RegNum, out.RegNum, build->GetConstantInt(1));
	}
	else
	{
		build->Emit((Token == TK_Incr) ? OP_ADDF_RK : OP_SUBF_RK, assign.RegNum, out.RegNum, build->GetConstantFloat(1.));
	}

	if (!pointer.Target)
	{
		build->Emit(ValueType->GetStoreOp(), pointer.RegNum, assign.RegNum, zero);
	}

	pointer.Free(build);
	assign.Free(build);
	return out;
}

//==========================================================================
//
// FxAssign
//
//==========================================================================

FxAssign::FxAssign(FxExpression *base, FxExpression *right)
: FxExpression(base->ScriptPosition), Base(base), Right(right)
{
	AddressRequested = false;
	AddressWritable = false;
}

FxAssign::~FxAssign()
{
	SAFE_DELETE(Base);
	SAFE_DELETE(Right);
}

bool FxAssign::RequestAddress(bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = AddressWritable;
	return true;
}

FxExpression *FxAssign::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Base, ctx);

	ValueType = Base->ValueType;

	SAFE_RESOLVE(Right, ctx);

	if (Base->IsNumeric() && Right->IsNumeric())
	{
		if (Right->ValueType != ValueType)
		{
			if (ValueType == TypeBool)
			{
				Right = new FxBoolCast(Right);
			}
			else if (ValueType->GetRegType() == REGT_INT)
			{
				Right = new FxIntCast(Right, ctx.FromDecorate);
			}
			else
			{
				Right = new FxFloatCast(Right);
			}
			SAFE_RESOLVE(Right, ctx);
		}
	}
	else if (Base->ValueType == Right->ValueType)
	{
		if (Base->ValueType->IsKindOf(RUNTIME_CLASS(PArray)))
		{
			ScriptPosition.Message(MSG_ERROR, "Cannot assign arrays");
			delete this;
			return nullptr;
		}
		if (Base->ValueType->IsKindOf(RUNTIME_CLASS(PStruct)))
		{
			ScriptPosition.Message(MSG_ERROR, "Struct assignment not implemented yet");
			delete this;
			return nullptr;
		}
		// Both types are the same so this is ok.
	}
	else if ((Base->ValueType == TypeState || Base->ValueType->IsKindOf(RUNTIME_CLASS(PPointer))) && Right->ValueType == TypeNullPtr)
	{
		// null pointers can be assigned to any other pointer
	}
	else if (Base->ValueType->IsKindOf(RUNTIME_CLASS(PClassPointer)))
	{
		// class pointers may be assignable so add a cast which performs a check.
		Right = new FxClassTypeCast(static_cast<PClassPointer *>(ValueType), Right);
		SAFE_RESOLVE(Right, ctx);
	}
	else if (Base->ValueType == TypeString && (Right->ValueType == TypeName || Right->ValueType == TypeSound))
	{
		Right = new FxStringCast(Right);
		SAFE_RESOLVE(Right, ctx);
	}
	else if (Base->ValueType == TypeName && Right->ValueType == TypeString)
	{
		Right = new FxNameCast(Right);
		SAFE_RESOLVE(Right, ctx);
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Assignment between incompatible types.");
		delete this;
		return nullptr;
	}
	if (!Base->RequestAddress(&AddressWritable) || !AddressWritable)
	{
		ScriptPosition.Message(MSG_ERROR, "Expression must be a modifiable value");
		delete this;
		return nullptr;
	}


	return this;
}

ExpEmit FxAssign::Emit(VMFunctionBuilder *build)
{
	static const BYTE loadops[] = { OP_LK, OP_LKF, OP_LKS, OP_LKP };
	assert(ValueType == Base->ValueType);
	assert(ValueType->GetRegType() == Right->ValueType->GetRegType());

	ExpEmit pointer = Base->Emit(build);
	Address = pointer;

	ExpEmit result = Right->Emit(build);
	assert(result.RegType <= REGT_TYPE);

	if (pointer.Target)
	{
		if (result.Konst)
		{
			build->Emit(loadops[result.RegType], pointer.RegNum, result.RegNum);
		}
		else
		{
			build->Emit(Right->ValueType->GetMoveOp(), pointer.RegNum, result.RegNum);
		}
	}
	else
	{
		if (result.Konst)
		{
			ExpEmit temp(build, result.RegType);
			build->Emit(loadops[result.RegType], temp.RegNum, result.RegNum);
			result.Free(build);
			result = temp;
		}

		build->Emit(ValueType->GetStoreOp(), pointer.RegNum, result.RegNum, build->GetConstantInt(0));
	}

	if (AddressRequested)
	{
		result.Free(build);
		return pointer;
	}

	pointer.Free(build);
	return result;
}

//==========================================================================
//
//	FxAssignSelf
//
//==========================================================================

FxAssignSelf::FxAssignSelf(const FScriptPosition &pos)
: FxExpression(pos)
{
	Assignment = nullptr;
}

FxExpression *FxAssignSelf::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();

	// This should never happen if FxAssignSelf is used correctly
	assert(Assignment != nullptr);

	ValueType = Assignment->ValueType;

	return this;
}

ExpEmit FxAssignSelf::Emit(VMFunctionBuilder *build)
{
	assert(ValueType = Assignment->ValueType);
	ExpEmit pointer = Assignment->Address; // FxAssign should have already emitted it
	if (!pointer.Target)
	{
		ExpEmit out(build, ValueType->GetRegType());
		build->Emit(ValueType->GetLoadOp(), out.RegNum, pointer.RegNum, build->GetConstantInt(0));
		return out;
	}
	else
	{
		return pointer;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxBinary::FxBinary(int o, FxExpression *l, FxExpression *r)
: FxExpression(l->ScriptPosition)
{
	Operator=o;
	left=l;
	right=r;
}

//==========================================================================
//
//
//
//==========================================================================

FxBinary::~FxBinary()
{
	SAFE_DELETE(left);
	SAFE_DELETE(right);
}

//==========================================================================
//
//
//
//==========================================================================

bool FxBinary::ResolveLR(FCompileContext& ctx, bool castnumeric)
{
	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	if (!left || !right)
	{
		delete this;
		return false;
	}

	if (left->ValueType == TypeBool && right->ValueType == TypeBool)
	{
		ValueType = TypeBool;
	}
	else if (left->ValueType == TypeName && right->ValueType == TypeName)
	{
		ValueType = TypeName;
	}
	else if (left->IsNumeric() && right->IsNumeric())
	{
		if (left->ValueType->GetRegType() == REGT_INT && right->ValueType->GetRegType() == REGT_INT)
		{
			ValueType = TypeSInt32;
		}
		else
		{
			ValueType = TypeFloat64;
		}
	}
	else if (left->ValueType->GetRegType() == REGT_POINTER)
	{
		if (left->ValueType == right->ValueType || right->ValueType == TypeNullPtr || left->ValueType == TypeNullPtr ||
			AreCompatiblePointerTypes(left->ValueType, right->ValueType))
		{
			// pointers can only be compared for equality.
			assert(Operator == TK_Eq || Operator == TK_Neq);
			ValueType = TypeBool;
		}
	}
	else
	{
		ValueType = TypeVoid;
	}
	assert(ValueType > nullptr && ValueType < (PType*)0xfffffffffffffff);

	if (castnumeric)
	{
		// later!
	}
	return true;
}

void FxBinary::Promote(FCompileContext &ctx)
{
	if (left->ValueType->GetRegType() == REGT_FLOAT && right->ValueType->GetRegType() == REGT_INT)
	{
		right = (new FxFloatCast(right))->Resolve(ctx);
	}
	else if (left->ValueType->GetRegType() == REGT_INT && right->ValueType->GetRegType() == REGT_FLOAT)
	{
		left = (new FxFloatCast(left))->Resolve(ctx);
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxAddSub::FxAddSub(int o, FxExpression *l, FxExpression *r)
: FxBinary(o, l, r)
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxAddSub::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	if (!ResolveLR(ctx, true)) return NULL;

	if (!IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return NULL;
	}
	else if (left->isConstant() && right->isConstant())
	{
		if (ValueType->GetRegType() == REGT_FLOAT)
		{
			double v;
			double v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
			double v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();

			v =	Operator == '+'? v1 + v2 : 
				Operator == '-'? v1 - v2 : 0;

			FxExpression *e = new FxConstant(v, ScriptPosition);
			delete this;
			return e;
		}
		else
		{
			int v;
			int v1 = static_cast<FxConstant *>(left)->GetValue().GetInt();
			int v2 = static_cast<FxConstant *>(right)->GetValue().GetInt();

			v =	Operator == '+'? v1 + v2 : 
				Operator == '-'? v1 - v2 : 0;

			FxExpression *e = new FxConstant(v, ScriptPosition);
			delete this;
			return e;

		}
	}
	Promote(ctx);
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxAddSub::Emit(VMFunctionBuilder *build)
{
	assert(Operator == '+' || Operator == '-');
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);
	if (Operator == '+')
	{
		// Since addition is commutative, only the second operand may be a constant.
		if (op1.Konst)
		{
			swapvalues(op1, op2);
		}
		assert(!op1.Konst);
		op1.Free(build);
		op2.Free(build);
		if (ValueType->GetRegType() == REGT_FLOAT)
		{
			assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
			ExpEmit to(build, REGT_FLOAT);
			build->Emit(op2.Konst ? OP_ADDF_RK : OP_ADDF_RR, to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
		else
		{
			assert(ValueType->GetRegType() == REGT_INT);
			assert(op1.RegType == REGT_INT && op2.RegType == REGT_INT);
			ExpEmit to(build, REGT_INT);
			build->Emit(op2.Konst ? OP_ADD_RK : OP_ADD_RR, to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
	}
	else
	{
		// Subtraction is not commutative, so either side may be constant (but not both).
		assert(!op1.Konst || !op2.Konst);
		op1.Free(build);
		op2.Free(build);
		if (ValueType->GetRegType() == REGT_FLOAT)
		{
			assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
			ExpEmit to(build, REGT_FLOAT);
			build->Emit(op1.Konst ? OP_SUBF_KR : op2.Konst ? OP_SUBF_RK : OP_SUBF_RR,
				to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
		else
		{
			assert(ValueType->GetRegType() == REGT_INT);
			assert(op1.RegType == REGT_INT && op2.RegType == REGT_INT);
			ExpEmit to(build, REGT_INT);
			build->Emit(op1.Konst ? OP_SUB_KR : op2.Konst ? OP_SUB_RK : OP_SUB_RR,
				to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxMulDiv::FxMulDiv(int o, FxExpression *l, FxExpression *r)
: FxBinary(o, l, r)
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxMulDiv::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	if (!ResolveLR(ctx, true)) return NULL;

	if (!IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return NULL;
	}
	else if (left->isConstant() && right->isConstant())
	{
		if (ValueType->GetRegType() == REGT_FLOAT)
		{
			double v;
			double v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
			double v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();

			if (Operator != '*' && v2 == 0)
			{
				ScriptPosition.Message(MSG_ERROR, "Division by 0");
				delete this;
				return NULL;
			}

			v =	Operator == '*'? v1 * v2 : 
				Operator == '/'? v1 / v2 : 
				Operator == '%'? fmod(v1, v2) : 0;

			FxExpression *e = new FxConstant(v, ScriptPosition);
			delete this;
			return e;
		}
		else
		{
			int v;
			int v1 = static_cast<FxConstant *>(left)->GetValue().GetInt();
			int v2 = static_cast<FxConstant *>(right)->GetValue().GetInt();

			if (Operator != '*' && v2 == 0)
			{
				ScriptPosition.Message(MSG_ERROR, "Division by 0");
				delete this;
				return NULL;
			}

			v =	Operator == '*'? v1 * v2 : 
				Operator == '/'? v1 / v2 : 
				Operator == '%'? v1 % v2 : 0;

			FxExpression *e = new FxConstant(v, ScriptPosition);
			delete this;
			return e;

		}
	}
	Promote(ctx);
	return this;
}


//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxMulDiv::Emit(VMFunctionBuilder *build)
{
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);

	if (Operator == '*')
	{
		// Multiplication is commutative, so only the second operand may be constant.
		if (op1.Konst)
		{
			swapvalues(op1, op2);
		}
		assert(!op1.Konst);
		op1.Free(build);
		op2.Free(build);
		if (ValueType->GetRegType() == REGT_FLOAT)
		{
			assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
			ExpEmit to(build, REGT_FLOAT);
			build->Emit(op2.Konst ? OP_MULF_RK : OP_MULF_RR, to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
		else
		{
			assert(ValueType->GetRegType() == REGT_INT);
			assert(op1.RegType == REGT_INT && op2.RegType == REGT_INT);
			ExpEmit to(build, REGT_INT);
			build->Emit(op2.Konst ? OP_MUL_RK : OP_MUL_RR, to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
	}
	else
	{
		// Division is not commutative, so either side may be constant (but not both).
		assert(!op1.Konst || !op2.Konst);
		assert(Operator == '%' || Operator == '/');
		op1.Free(build);
		op2.Free(build);
		if (ValueType->GetRegType() == REGT_FLOAT)
		{
			assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
			ExpEmit to(build, REGT_FLOAT);
			build->Emit(Operator == '/' ? (op1.Konst ? OP_DIVF_KR : op2.Konst ? OP_DIVF_RK : OP_DIVF_RR)
				: (op1.Konst ? OP_MODF_KR : op2.Konst ? OP_MODF_RK : OP_MODF_RR),
				to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
		else
		{
			assert(ValueType->GetRegType() == REGT_INT);
			assert(op1.RegType == REGT_INT && op2.RegType == REGT_INT);
			ExpEmit to(build, REGT_INT);
			build->Emit(Operator == '/' ? (op1.Konst ? OP_DIV_KR : op2.Konst ? OP_DIV_RK : OP_DIV_RR)
				: (op1.Konst ? OP_MOD_KR : op2.Konst ? OP_MOD_RK : OP_MOD_RR),
				to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxPow::FxPow(FxExpression *l, FxExpression *r)
	: FxBinary(TK_MulMul, new FxFloatCast(l), new FxFloatCast(r))
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxPow::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	if (!ResolveLR(ctx, true)) return nullptr;

	if (!IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return nullptr;
	}
	if (left->isConstant() && right->isConstant())
	{
		double v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
		double v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();
		return new FxConstant(g_pow(v1, v2), left->ScriptPosition);
	}
	return this;
}


//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxPow::Emit(VMFunctionBuilder *build)
{
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);

	// Pow is not commutative, so either side may be constant (but not both).
	assert(!op1.Konst || !op2.Konst);
	op1.Free(build);
	op2.Free(build);
	assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
	ExpEmit to(build, REGT_FLOAT);
	build->Emit((op1.Konst ? OP_POWF_KR : op2.Konst ? OP_POWF_RK : OP_POWF_RR),	to.RegNum, op1.RegNum, op2.RegNum);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxCompareRel::FxCompareRel(int o, FxExpression *l, FxExpression *r)
: FxBinary(o, l, r)
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxCompareRel::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	if (!ResolveLR(ctx, true)) return NULL;

	if (!IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return NULL;
	}
	else if (left->isConstant() && right->isConstant())
	{
		int v;

		if (ValueType->GetRegType() == REGT_FLOAT)
		{
			double v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
			double v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();
			v =	Operator == '<'? v1 < v2 : 
				Operator == '>'? v1 > v2 : 
				Operator == TK_Geq? v1 >= v2 : 
				Operator == TK_Leq? v1 <= v2 : 0;
		}
		else
		{
			int v1 = static_cast<FxConstant *>(left)->GetValue().GetInt();
			int v2 = static_cast<FxConstant *>(right)->GetValue().GetInt();
			v =	Operator == '<'? v1 < v2 : 
				Operator == '>'? v1 > v2 : 
				Operator == TK_Geq? v1 >= v2 : 
				Operator == TK_Leq? v1 <= v2 : 0;
		}
		FxExpression *e = new FxConstant(v, ScriptPosition);
		delete this;
		return e;
	}
	Promote(ctx);
	ValueType = TypeBool;
	return this;
}


//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxCompareRel::Emit(VMFunctionBuilder *build)
{
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);
	assert(op1.RegType == op2.RegType);
	assert(op1.RegType == REGT_INT || op1.RegType == REGT_FLOAT);
	assert(!op1.Konst || !op2.Konst);
	assert(Operator == '<' || Operator == '>' || Operator == TK_Geq || Operator == TK_Leq);
	static const VM_UBYTE InstrMap[][4] =
	{
		{ OP_LT_RR, OP_LTF_RR, 0 },	// <
		{ OP_LE_RR, OP_LEF_RR, 1 },	// >
		{ OP_LT_RR, OP_LTF_RR, 1 },	// >=
		{ OP_LE_RR, OP_LEF_RR, 0 }	// <=
	};
	int instr, check, index;
	ExpEmit to(build, REGT_INT);

	index = Operator == '<' ? 0 :
			Operator == '>' ? 1 :
			Operator == TK_Geq ? 2 : 3;
	instr = InstrMap[index][op1.RegType == REGT_INT ? 0 : 1];
	check = InstrMap[index][2];
	if (op2.Konst)
	{
		instr += 1;
	}
	else
	{
		op2.Free(build);
	}
	if (op1.Konst)
	{
		instr += 2;
	}
	else
	{
		op1.Free(build);
	}

	// See FxBoolCast for comments, since it's the same thing.
	build->Emit(OP_LI, to.RegNum, 0, 0);
	build->Emit(instr, check, op1.RegNum, op2.RegNum);
	build->Emit(OP_JMP, 1);
	build->Emit(OP_LI, to.RegNum, 1);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxCompareEq::FxCompareEq(int o, FxExpression *l, FxExpression *r)
: FxBinary(o, l, r)
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxCompareEq::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	if (!ResolveLR(ctx, true)) return NULL;

	if (!left || !right)
	{
		delete this;
		return NULL;
	}

	if (!IsNumeric() && !IsPointer() && ValueType != TypeName)
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return NULL;
	}

	if (Operator == TK_ApproxEq && ValueType->GetRegType() != REGT_FLOAT) Operator = TK_Eq;
	if (left->isConstant() && right->isConstant())
	{
		int v;

		if (ValueType->GetRegType() == REGT_FLOAT)
		{
			double v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
			double v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();
			v = Operator == TK_Eq? v1 == v2 : Operator == TK_Neq? v1 != v2 : fabs(v1-v2) < VM_EPSILON;
		}
		else
		{
			int v1 = static_cast<FxConstant *>(left)->GetValue().GetInt();
			int v2 = static_cast<FxConstant *>(right)->GetValue().GetInt();
			v = Operator == TK_Eq? v1 == v2 : v1 != v2;
		}
		FxExpression *e = new FxConstant(v, ScriptPosition);
		delete this;
		return e;
	}
	Promote(ctx);
	ValueType = TypeBool;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxCompareEq::Emit(VMFunctionBuilder *build)
{
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);
	assert(op1.RegType == op2.RegType);
	assert(op1.RegType == REGT_INT || op1.RegType == REGT_FLOAT || op1.RegType == REGT_POINTER);
	int instr;

	// Only the second operand may be constant.
	if (op1.Konst)
	{
		swapvalues(op1, op2);
	}
	assert(!op1.Konst);

	ExpEmit to(build, REGT_INT);

	instr = op1.RegType == REGT_INT ? OP_EQ_R :
			op1.RegType == REGT_FLOAT ? OP_EQF_R :
			OP_EQA_R;
	op1.Free(build);
	if (!op2.Konst)
	{
		op2.Free(build);
	}
	else
	{
		instr += 1;
	}

	// See FxUnaryNotBoolean for comments, since it's the same thing.
	build->Emit(OP_LI, to.RegNum, 0, 0);
	build->Emit(instr, Operator == TK_ApproxEq? CMP_APPROX : Operator != TK_Eq, op1.RegNum, op2.RegNum);
	build->Emit(OP_JMP, 1);
	build->Emit(OP_LI, to.RegNum, 1);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxBinaryInt::FxBinaryInt(int o, FxExpression *l, FxExpression *r)
: FxBinary(o, l, r)
{
	ValueType = TypeSInt32;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxBinaryInt::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	if (!ResolveLR(ctx, false)) return NULL;

	if (ValueType->GetRegType() == REGT_FLOAT && ctx.FromDecorate)
	{
		// For DECORATE which allows floats here. ZScript does not.
		if (left->ValueType->GetRegType() != REGT_INT)
		{
			left = new FxIntCast(left, ctx.FromDecorate);
			left = left->Resolve(ctx);
		}
		if (right->ValueType->GetRegType() != REGT_INT)
		{
			right = new FxIntCast(right, ctx.FromDecorate);
			right = right->Resolve(ctx);
		}
		if (left == NULL || right == NULL)
		{
			delete this;
			return NULL;
		}
		ValueType = TypeSInt32;
	}

	if (ValueType->GetRegType() != REGT_INT)
	{
		ScriptPosition.Message(MSG_ERROR, "Integer type expected");
		delete this;
		return NULL;
	}
	else if (left->isConstant() && right->isConstant())
	{
		int v1 = static_cast<FxConstant *>(left)->GetValue().GetInt();
		int v2 = static_cast<FxConstant *>(right)->GetValue().GetInt();

		FxExpression *e = new FxConstant(
			Operator == TK_LShift? v1 << v2 : 
			Operator == TK_RShift? v1 >> v2 : 
			Operator == TK_URShift? int((unsigned int)(v1) >> v2) : 
			Operator == '&'? v1 & v2 : 
			Operator == '|'? v1 | v2 : 
			Operator == '^'? v1 ^ v2 : 0, ScriptPosition);

		delete this;
		return e;
	}
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxBinaryInt::Emit(VMFunctionBuilder *build)
{
	assert(left->ValueType->GetRegType() == REGT_INT);
	assert(right->ValueType->GetRegType() == REGT_INT);
	static const VM_UBYTE InstrMap[][4] =
	{
		{ OP_SLL_RR, OP_SLL_KR, OP_SLL_RI },	// TK_LShift
		{ OP_SRA_RR, OP_SRA_KR, OP_SRA_RI },	// TK_RShift
		{ OP_SRL_RR, OP_SRL_KR, OP_SRL_RI },	// TK_URShift
		{ OP_AND_RR, 0,         OP_AND_RK },	// '&'
		{ OP_OR_RR,  0,			OP_OR_RK  },	// '|'
		{ OP_XOR_RR, 0,         OP_XOR_RK },	// '^'
	};
	int index, instr, rop;
	ExpEmit op1, op2;

	index = Operator == TK_LShift ? 0 :
			Operator == TK_RShift ? 1 :
			Operator == TK_URShift ? 2 :
			Operator == '&' ? 3 :
			Operator == '|' ? 4 :
			Operator == '^' ? 5 : -1;
	assert(index >= 0);
	op1 = left->Emit(build);
	if (index < 3)
	{ // Shift instructions use right-hand immediates instead of constant registers.
		if (right->isConstant())
		{
			rop = static_cast<FxConstant *>(right)->GetValue().GetInt();
			op2.Konst = true;
		}
		else
		{
			op2 = right->Emit(build);
			assert(!op2.Konst);
			op2.Free(build);
			rop = op2.RegNum;
		}
	}
	else
	{ // The other operators only take a constant on the right-hand side.
		op2 = right->Emit(build);
		if (op1.Konst)
		{
			swapvalues(op1, op2);
		}
		assert(!op1.Konst);
		rop = op2.RegNum;
		op2.Free(build);
	}
	if (!op1.Konst)
	{
		op1.Free(build);
		if (!op2.Konst)
		{
			instr = InstrMap[index][0];
		}
		else
		{
			instr = InstrMap[index][2];
		}
	}
	else
	{
		assert(!op2.Konst);
		instr = InstrMap[index][1];
	}
	assert(instr != 0);
	ExpEmit to(build, REGT_INT);
	build->Emit(instr, to.RegNum, op1.RegNum, rop);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxLtGtEq::FxLtGtEq(FxExpression *l, FxExpression *r)
	: FxBinary(TK_LtGtEq, l, r)
{
	ValueType = TypeSInt32;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxLtGtEq::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	if (!ResolveLR(ctx, false)) return NULL;

	if (!left->IsNumeric() || !right->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "<>= expects two numeric operands");
		delete this;
		return nullptr;
	}
	if (left->ValueType->GetRegType() != right->ValueType->GetRegType())
	{
		if (left->ValueType->GetRegType() == REGT_INT)
		{
			left = new FxFloatCast(left);
			SAFE_RESOLVE(left, ctx);
		}
		if (right->ValueType->GetRegType() == REGT_INT)
		{
			right = new FxFloatCast(left);
			SAFE_RESOLVE(left, ctx);
		}
	}

	else if (left->isConstant() && right->isConstant())
	{
		// let's cut this short and always compare doubles. For integers the result will be exactly the same as with an integer comparison, either signed or unsigned.
		auto v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
		auto v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();
		auto e = new FxConstant(v1 < v2 ? -1 : v1 > v2 ? 1 : 0, ScriptPosition);
		delete this;
		return e;
	}
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxLtGtEq::Emit(VMFunctionBuilder *build)
{
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);

	assert(op1.RegType == op2.RegType);
	assert(op1.RegType == REGT_INT || op1.RegType == REGT_FLOAT);
	assert(!op1.Konst || !op2.Konst);

	ExpEmit to(build, REGT_INT);

	int instr = op1.RegType == REGT_INT ? (left->ValueType == TypeUInt32? OP_LTU_RR : OP_LT_RR) : OP_LTF_RR;
	if (op1.Konst) instr += 2;
	if (op2.Konst) instr++;


	build->Emit(OP_LI, to.RegNum, 1);										// default to 1
	build->Emit(instr, 0, op1.RegNum, op2.RegNum);							// if (left < right)
	auto j1 = build->Emit(OP_JMP, 1);
	build->Emit(OP_LI, to.RegNum, -1);										// result is -1
	auto j2 = build->Emit(OP_JMP, 1);										// jump to end
	build->BackpatchToHere(j1);
	build->Emit(instr + OP_LE_RR - OP_LT_RR, 0, op1.RegNum, op2.RegNum);	// if (left == right)
	auto j3 = build->Emit(OP_JMP, 1);
	build->Emit(OP_LI, to.RegNum, 0);										// result is 0
	build->BackpatchToHere(j2);
	build->BackpatchToHere(j3);

	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxBinaryLogical::FxBinaryLogical(int o, FxExpression *l, FxExpression *r)
: FxExpression(l->ScriptPosition)
{
	Operator=o;
	left=l;
	right=r;
	ValueType = TypeBool;
}

//==========================================================================
//
//
//
//==========================================================================

FxBinaryLogical::~FxBinaryLogical()
{
	SAFE_DELETE(left);
	SAFE_DELETE(right);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxBinaryLogical::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	ABORT(right && left);

	if (left->ValueType != TypeBool)
	{
		left = new FxBoolCast(left);
		SAFE_RESOLVE(left, ctx);
	}
	if (right->ValueType != TypeBool)
	{
		right = new FxBoolCast(right);
		SAFE_RESOLVE(right, ctx);
	}

	int b_left=-1, b_right=-1;
	if (left->isConstant()) b_left = static_cast<FxConstant *>(left)->GetValue().GetBool();
	if (right->isConstant()) b_right = static_cast<FxConstant *>(right)->GetValue().GetBool();

	// Do some optimizations. This will throw out all sub-expressions that are not
	// needed to retrieve the final result.
	if (Operator == TK_AndAnd)
	{
		if (b_left==0 || b_right==0)
		{
			FxExpression *x = new FxConstant(true, ScriptPosition);
			delete this;
			return x;
		}
		else if (b_left==1 && b_right==1)
		{
			FxExpression *x = new FxConstant(false, ScriptPosition);
			delete this;
			return x;
		}
		else if (b_left==1)
		{
			FxExpression *x = right;
			right=NULL;
			delete this;
			return x;
		}
		else if (b_right==1)
		{
			FxExpression *x = left;
			left=NULL;
			delete this;
			return x;
		}
	}
	else if (Operator == TK_OrOr)
	{
		if (b_left==1 || b_right==1)
		{
			FxExpression *x = new FxConstant(true, ScriptPosition);
			delete this;
			return x;
		}
		if (b_left==0 && b_right==0)
		{
			FxExpression *x = new FxConstant(false, ScriptPosition);
			delete this;
			return x;
		}
		else if (b_left==0)
		{
			FxExpression *x = right;
			right=NULL;
			delete this;
			return x;
		}
		else if (b_right==0)
		{
			FxExpression *x = left;
			left=NULL;
			delete this;
			return x;
		}
	}
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxBinaryLogical::Emit(VMFunctionBuilder *build)
{
	// This is not the "right" way to do these, but it works for now.
	// (Problem: No information sharing is done between nodes to reduce the
	// code size if you have something like a1 && a2 && a3 && ... && an.)
	assert(left->ValueType->GetRegType() == REGT_INT && right->ValueType->GetRegType() == REGT_INT);
	ExpEmit op1 = left->Emit(build);
	assert(!op1.Konst);
	int zero = build->GetConstantInt(0);
	op1.Free(build);

	if (Operator == TK_AndAnd)
	{
		build->Emit(OP_EQ_K, 1, op1.RegNum, zero);
		// If op1 is 0, skip evaluation of op2.
		size_t patchspot = build->Emit(OP_JMP, 0, 0, 0);

		// Evaluate op2.
		ExpEmit op2 = right->Emit(build);
		assert(!op2.Konst);
		op2.Free(build);

		ExpEmit to(build, REGT_INT);
		build->Emit(OP_EQ_K, 1, op2.RegNum, zero);
		build->Emit(OP_JMP, 2);
		build->Emit(OP_LI, to.RegNum, 1);
		build->Emit(OP_JMP, 1);
		size_t target = build->Emit(OP_LI, to.RegNum, 0);
		build->Backpatch(patchspot, target);
		return to;
	}
	else
	{
		assert(Operator == TK_OrOr);
		build->Emit(OP_EQ_K, 0, op1.RegNum, zero);
		// If op1 is not 0, skip evaluation of op2.
		size_t patchspot = build->Emit(OP_JMP, 0, 0, 0);

		// Evaluate op2.
		ExpEmit op2 = right->Emit(build);
		assert(!op2.Konst);
		op2.Free(build);

		ExpEmit to(build, REGT_INT);
		build->Emit(OP_EQ_K, 0, op2.RegNum, zero);
		build->Emit(OP_JMP, 2);
		build->Emit(OP_LI, to.RegNum, 0);
		build->Emit(OP_JMP, 1);
		size_t target = build->Emit(OP_LI, to.RegNum, 1);
		build->Backpatch(patchspot, target);
		return to;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxConditional::FxConditional(FxExpression *c, FxExpression *t, FxExpression *f)
: FxExpression(c->ScriptPosition)
{
	condition = c;
	truex=t;
	falsex=f;
}

//==========================================================================
//
//
//
//==========================================================================

FxConditional::~FxConditional()
{
	SAFE_DELETE(condition);
	SAFE_DELETE(truex);
	SAFE_DELETE(falsex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxConditional::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	RESOLVE(condition, ctx);
	RESOLVE(truex, ctx);
	RESOLVE(falsex, ctx);
	ABORT(condition && truex && falsex);

	if (truex->ValueType == TypeBool && falsex->ValueType == TypeBool)
		ValueType = TypeBool;
	else if (truex->ValueType->GetRegType() == REGT_INT && falsex->ValueType->GetRegType() == REGT_INT)
		ValueType = TypeSInt32;
	else if (truex->IsNumeric() && falsex->IsNumeric())
		ValueType = TypeFloat64;
	//else if (truex->ValueType != falsex->ValueType)

	if (condition->ValueType != TypeBool)
	{
		condition = new FxBoolCast(condition);
		SAFE_RESOLVE(condition, ctx);
	}

	if (condition->isConstant())
	{
		ExpVal condval = static_cast<FxConstant *>(condition)->GetValue();
		bool result = condval.GetBool();

		FxExpression *e = result? truex:falsex;
		delete (result? falsex:truex);
		falsex = truex = NULL;
		delete this;
		return e;
	}

	if (ValueType->GetRegType() == REGT_FLOAT)
	{
		if (truex->ValueType->GetRegType() != REGT_FLOAT)
		{
			truex = new FxFloatCast(truex);
			RESOLVE(truex, ctx);
		}
		if (falsex->ValueType->GetRegType() != REGT_FLOAT)
		{
			falsex = new FxFloatCast(falsex);
			RESOLVE(falsex, ctx);
		}
	}

	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxConditional::Emit(VMFunctionBuilder *build)
{
	size_t truejump, falsejump;
	ExpEmit out;

	// The true and false expressions ought to be assigned to the
	// same temporary instead of being copied to it. Oh well; good enough
	// for now.
	ExpEmit cond = condition->Emit(build);
	assert(cond.RegType == REGT_INT && !cond.Konst);

	// Test condition.
	build->Emit(OP_EQ_K, 1, cond.RegNum, build->GetConstantInt(0));
	falsejump = build->Emit(OP_JMP, 0);

	// Evaluate true expression.
	if (truex->isConstant() && truex->ValueType->GetRegType() == REGT_INT)
	{
		out = ExpEmit(build, REGT_INT);
		build->EmitLoadInt(out.RegNum, static_cast<FxConstant *>(truex)->GetValue().GetInt());
	}
	else
	{
		ExpEmit trueop = truex->Emit(build);
		if (trueop.Konst)
		{
			assert(trueop.RegType == REGT_FLOAT);
			out = ExpEmit(build, REGT_FLOAT);
			build->Emit(OP_LKF, out.RegNum, trueop.RegNum);
		}
		else
		{
			// Use the register returned by the true condition as the
			// target for the false condition.
			out = trueop;
		}
	}
	// Make sure to skip the false path.
	truejump = build->Emit(OP_JMP, 0);

	// Evaluate false expression.
	build->BackpatchToHere(falsejump);
	if (falsex->isConstant() && falsex->ValueType->GetRegType() == REGT_INT)
	{
		build->EmitLoadInt(out.RegNum, static_cast<FxConstant *>(falsex)->GetValue().GetInt());
	}
	else
	{
		ExpEmit falseop = falsex->Emit(build);
		if (falseop.Konst)
		{
			assert(falseop.RegType == REGT_FLOAT);
			build->Emit(OP_LKF, out.RegNum, falseop.RegNum);
		}
		else
		{
			// Move result from the register returned by "false" to the one
			// returned by "true" so that only one register is returned by
			// this tree.
			falseop.Free(build);
			if (falseop.RegType == REGT_INT)
			{
				build->Emit(OP_MOVE, out.RegNum, falseop.RegNum, 0);
			}
			else
			{
				assert(falseop.RegType == REGT_FLOAT);
				build->Emit(OP_MOVEF, out.RegNum, falseop.RegNum, 0);
			}
		}
	}
	build->BackpatchToHere(truejump);

	return out;
}

//==========================================================================
//
//
//
//==========================================================================
FxAbs::FxAbs(FxExpression *v)
: FxExpression(v->ScriptPosition)
{
	val = v;
	ValueType = v->ValueType;
}

//==========================================================================
//
//
//
//==========================================================================

FxAbs::~FxAbs()
{
	SAFE_DELETE(val);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxAbs::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(val, ctx);


	if (!val->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return NULL;
	}
	else if (val->isConstant())
	{
		ExpVal value = static_cast<FxConstant *>(val)->GetValue();
		switch (value.Type->GetRegType())
		{
		case REGT_INT:
			value.Int = abs(value.Int);
			break;

		case REGT_FLOAT:
			value.Float = fabs(value.Float);
			break;

		default:
			// shouldn't happen
			delete this;
			return NULL;
		}
		FxExpression *x = new FxConstant(value, ScriptPosition);
		delete this;
		return x;
	}
	ValueType = val->ValueType;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxAbs::Emit(VMFunctionBuilder *build)
{
	ExpEmit absofsteal = val->Emit(build);
	assert(!absofsteal.Konst);
	ExpEmit out(build, absofsteal.RegType);
	if (absofsteal.RegType == REGT_INT)
	{
		build->Emit(OP_ABS, out.RegNum, absofsteal.RegNum, 0);
	}
	else
	{
		assert(absofsteal.RegType == REGT_FLOAT);
		build->Emit(OP_FLOP, out.RegNum, absofsteal.RegNum, FLOP_ABS);
	}
	return out;
}

//==========================================================================
//
//
//
//==========================================================================
FxATan2::FxATan2(FxExpression *y, FxExpression *x, const FScriptPosition &pos)
: FxExpression(pos)
{
	yval = y;
	xval = x;
}

//==========================================================================
//
//
//
//==========================================================================
FxATan2::~FxATan2()
{
	SAFE_DELETE(yval);
	SAFE_DELETE(xval);
}

//==========================================================================
//
//
//
//==========================================================================
FxExpression *FxATan2::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(yval, ctx);
	SAFE_RESOLVE(xval, ctx);

	if (!yval->IsNumeric() || !xval->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "numeric value expected for parameter");
		delete this;
		return NULL;
	}
	if (yval->isConstant() && xval->isConstant())
	{
		double y = static_cast<FxConstant *>(yval)->GetValue().GetFloat();
		double x = static_cast<FxConstant *>(xval)->GetValue().GetFloat();
		FxExpression *z = new FxConstant(g_atan2(y, x) * (180 / M_PI), ScriptPosition);
		delete this;
		return z;
	}
	if (yval->ValueType->GetRegType() != REGT_FLOAT && !yval->isConstant())
	{
		yval = new FxFloatCast(yval);
	}
	if (xval->ValueType->GetRegType() != REGT_FLOAT && !xval->isConstant())
	{
		xval = new FxFloatCast(xval);
	}
	ValueType = TypeFloat64;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================
ExpEmit FxATan2::Emit(VMFunctionBuilder *build)
{
	ExpEmit yreg = ToReg(build, yval);
	ExpEmit xreg = ToReg(build, xval);
	yreg.Free(build);
	xreg.Free(build);
	ExpEmit out(build, REGT_FLOAT);
	build->Emit(OP_ATAN2, out.RegNum, yreg.RegNum, xreg.RegNum);
	return out;
}

//==========================================================================
//
// The atan2 opcode only takes registers as parameters, so any constants
// must be loaded into registers first.
//
//==========================================================================
ExpEmit FxATan2::ToReg(VMFunctionBuilder *build, FxExpression *val)
{
	if (val->isConstant())
	{
		ExpEmit reg(build, REGT_FLOAT);
		build->Emit(OP_LKF, reg.RegNum, build->GetConstantFloat(static_cast<FxConstant*>(val)->GetValue().GetFloat()));
		return reg;
	}
	return val->Emit(build);
}

//==========================================================================
//
//
//
//==========================================================================
FxMinMax::FxMinMax(TArray<FxExpression*> &expr, FName type, const FScriptPosition &pos)
: FxExpression(pos), Type(type)
{
	assert(expr.Size() > 0);
	assert(type == NAME_Min || type == NAME_Max);

	choices.Resize(expr.Size());
	for (unsigned i = 0; i < expr.Size(); ++i)
	{
		choices[i] = expr[i];
	}
}

//==========================================================================
//
//
//
//==========================================================================
FxExpression *FxMinMax::Resolve(FCompileContext &ctx)
{
	unsigned int i;
	int intcount, floatcount;

	CHECKRESOLVED();

	// Determine if float or int
	intcount = floatcount = 0;
	for (i = 0; i < choices.Size(); ++i)
	{
		RESOLVE(choices[i], ctx);
		ABORT(choices[i]);

		if (choices[i]->ValueType->GetRegType() == REGT_FLOAT)
		{
			floatcount++;
		}
		else if (choices[i]->ValueType->GetRegType() == REGT_INT && choices[i]->ValueType != TypeName)
		{
			intcount++;
		}
		else
		{
			ScriptPosition.Message(MSG_ERROR, "Arguments must be of type int or float");
			delete this;
			return NULL;
		}
	}
	if (floatcount != 0)
	{
		ValueType = TypeFloat64;
		if (intcount != 0)
		{ // There are some ints that need to be cast to floats
			for (i = 0; i < choices.Size(); ++i)
			{
				if (choices[i]->ValueType->GetRegType() == REGT_INT)
				{
					choices[i] = new FxFloatCast(choices[i]);
					RESOLVE(choices[i], ctx);
					ABORT(choices[i]);
				}
			}
		}
	}
	else
	{
		ValueType = TypeSInt32;
	}

	// If at least two arguments are constants, they can be solved now.

	// Look for first constant
	for (i = 0; i < choices.Size(); ++i)
	{
		if (choices[i]->isConstant())
		{
			ExpVal best = static_cast<FxConstant *>(choices[i])->GetValue();
			// Compare against remaining constants, which are removed.
			// The best value gets stored in this one.
			for (unsigned j = i + 1; j < choices.Size(); )
			{
				if (!choices[j]->isConstant())
				{
					j++;
				}
				else
				{
					ExpVal value = static_cast<FxConstant *>(choices[j])->GetValue();
					assert(value.Type == ValueType);
					if (Type == NAME_Min)
					{
						if (value.Type->GetRegType() == REGT_FLOAT)
						{
							if (value.Float < best.Float)
							{
								best.Float = value.Float;
							}
						}
						else
						{
							if (value.Int < best.Int)
							{
								best.Int = value.Int;
							}
						}
					}
					else
					{
						if (value.Type->GetRegType() == REGT_FLOAT)
						{
							if (value.Float > best.Float)
							{
								best.Float = value.Float;
							}
						}
						else
						{
							if (value.Int > best.Int)
							{
								best.Int = value.Int;
							}
						}
					}
					delete choices[j];
					choices[j] = NULL;
					choices.Delete(j);
				}
			}
			FxExpression *x = new FxConstant(best, ScriptPosition);
			if (i == 0 && choices.Size() == 1)
			{ // Every choice was constant
				delete this;
				return x;
			}
			delete choices[i];
			choices[i] = x;
			break;
		}
	}
	return this;
}

//==========================================================================
//
//
//
//==========================================================================
static void EmitLoad(VMFunctionBuilder *build, const ExpEmit resultreg, const ExpVal &value)
{
	if (resultreg.RegType == REGT_FLOAT)
	{
		build->Emit(OP_LKF, resultreg.RegNum, build->GetConstantFloat(value.GetFloat()));
	}
	else
	{
		build->EmitLoadInt(resultreg.RegNum, value.GetInt());
	}
}

ExpEmit FxMinMax::Emit(VMFunctionBuilder *build)
{
	unsigned i;
	int opcode, opA;

	assert(choices.Size() > 0);
	assert(OP_LTF_RK == OP_LTF_RR+1);
	assert(OP_LT_RK == OP_LT_RR+1);
	assert(OP_LEF_RK == OP_LEF_RR+1);
	assert(OP_LE_RK == OP_LE_RR+1);

	if (Type == NAME_Min)
	{
		opcode = ValueType->GetRegType() == REGT_FLOAT ? OP_LEF_RR : OP_LE_RR;
		opA = 1;
	}
	else
	{
		opcode = ValueType->GetRegType() == REGT_FLOAT ? OP_LTF_RR : OP_LT_RR;
		opA = 0;
	}

	ExpEmit bestreg;

	// Get first value into a register. This will also be the result register.
	if (choices[0]->isConstant())
	{
		bestreg = ExpEmit(build, ValueType->GetRegType());
		EmitLoad(build, bestreg, static_cast<FxConstant *>(choices[0])->GetValue());
	}
	else
	{
		bestreg = choices[0]->Emit(build);
	}

	// Compare every choice. Better matches get copied to the bestreg.
	for (i = 1; i < choices.Size(); ++i)
	{
		ExpEmit checkreg = choices[i]->Emit(build);
		assert(checkreg.RegType == bestreg.RegType);
		build->Emit(opcode + checkreg.Konst, opA, bestreg.RegNum, checkreg.RegNum);
		build->Emit(OP_JMP, 1);
		if (checkreg.Konst)
		{
			build->Emit(bestreg.RegType == REGT_FLOAT ? OP_LKF : OP_LK, bestreg.RegNum, checkreg.RegNum);
		}
		else
		{
			build->Emit(bestreg.RegType == REGT_FLOAT ? OP_MOVEF : OP_MOVE, bestreg.RegNum, checkreg.RegNum, 0);
			checkreg.Free(build);
		}
	}
	return bestreg;
}

//==========================================================================
//
//
//
//==========================================================================
FxRandom::FxRandom(FRandom * r, FxExpression *mi, FxExpression *ma, const FScriptPosition &pos, bool nowarn)
: FxExpression(pos)
{
	EmitTail = false;
	if (mi != NULL && ma != NULL)
	{
		min = new FxIntCast(mi, nowarn);
		max = new FxIntCast(ma, nowarn);
	}
	else min = max = NULL;
	rng = r;
	ValueType = TypeSInt32;
}

//==========================================================================
//
//
//
//==========================================================================

FxRandom::~FxRandom()
{
	SAFE_DELETE(min);
	SAFE_DELETE(max);
}

//==========================================================================
//
//
//
//==========================================================================

PPrototype *FxRandom::ReturnProto()
{
	EmitTail = true;
	return FxExpression::ReturnProto();
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxRandom::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	if (min && max)
	{
		RESOLVE(min, ctx);
		RESOLVE(max, ctx);
		ABORT(min && max);
		assert(min->ValueType == ValueType);
		assert(max->ValueType == ValueType);
	}
	return this;
};


//==========================================================================
//
//
//
//==========================================================================

int DecoRandom(VMFrameStack *stack, VMValue *param, int numparam, VMReturn *ret, int numret)
{
	assert(numparam >= 1 && numparam <= 3);
	FRandom *rng = reinterpret_cast<FRandom *>(param[0].a);
	if (numparam == 1)
	{
		ACTION_RETURN_INT((*rng)());
	}
	else if (numparam == 2)
	{
		int maskval = param[1].i;
		ACTION_RETURN_INT(rng->Random2(maskval));
	}
	else if (numparam == 3)
	{
		int min = param[1].i, max = param[2].i;
		if (max < min)
		{
			swapvalues(max, min);
		}
		ACTION_RETURN_INT((*rng)(max - min + 1) + min);
	}

	// Shouldn't happen
	return 0;
}

ExpEmit FxRandom::Emit(VMFunctionBuilder *build)
{
	// Call DecoRandom to generate a random number.
	VMFunction *callfunc;
	PSymbol *sym = FindDecorateBuiltinFunction(NAME_DecoRandom, DecoRandom);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != NULL);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	int opcode = (EmitTail ? OP_TAIL_K : OP_CALL_K);

	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(rng, ATAG_RNG));
	if (min != NULL && max != NULL)
	{
		EmitParameter(build, min, ScriptPosition);
		EmitParameter(build, max, ScriptPosition);
		build->Emit(opcode, build->GetConstantAddress(callfunc, ATAG_OBJECT), 3, 1);
	}
	else
	{
		build->Emit(opcode, build->GetConstantAddress(callfunc, ATAG_OBJECT), 1, 1);
	}

	if (EmitTail)
	{
		ExpEmit call;
		call.Final = true;
		return call;
	}

	ExpEmit out(build, REGT_INT);
	build->Emit(OP_RESULT, 0, REGT_INT, out.RegNum);
	return out;
}

//==========================================================================
//
//
//
//==========================================================================
FxRandomPick::FxRandomPick(FRandom *r, TArray<FxExpression*> &expr, bool floaty, const FScriptPosition &pos, bool nowarn)
: FxExpression(pos)
{
	assert(expr.Size() > 0);
	choices.Resize(expr.Size());
	for (unsigned int index = 0; index < expr.Size(); index++)
	{
		if (floaty)
		{
			choices[index] = new FxFloatCast(expr[index]);
		}
		else
		{
			choices[index] = new FxIntCast(expr[index], nowarn);
		}

	}
	rng = r;
	if (floaty)
	{
		ValueType = TypeFloat64;
	}
	else
	{
		ValueType = TypeSInt32;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxRandomPick::~FxRandomPick()
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxRandomPick::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	for (unsigned int index = 0; index < choices.Size(); index++)
	{
		RESOLVE(choices[index], ctx);
		ABORT(choices[index]);
		assert(choices[index]->ValueType == ValueType);
	}
	return this;
};


//==========================================================================
//
// FxPick :: Emit
//
// The expression:
//   a = pick[rng](i_0, i_1, i_2, ..., i_n)
//   [where i_x is a complete expression and not just a value]
// is syntactic sugar for:
//
//   switch(random[rng](0, n)) {
//     case 0: a = i_0;
//     case 1: a = i_1;
//     case 2: a = i_2;
//     ...
//     case n: a = i_n;
//   }
//
//==========================================================================

ExpEmit FxRandomPick::Emit(VMFunctionBuilder *build)
{
	unsigned i;

	assert(choices.Size() > 0);

	// Call DecoRandom to generate a random number.
	VMFunction *callfunc;
	PSymbol *sym = FindDecorateBuiltinFunction(NAME_DecoRandom, DecoRandom);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != NULL);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(rng, ATAG_RNG));
	build->EmitParamInt(0);
	build->EmitParamInt(choices.Size() - 1);
	build->Emit(OP_CALL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 3, 1);

	ExpEmit resultreg(build, REGT_INT);
	build->Emit(OP_RESULT, 0, REGT_INT, resultreg.RegNum);
	build->Emit(OP_IJMP, resultreg.RegNum, 0);

	// Free the result register now. The simple code generation algorithm should
	// automatically pick it as the destination register for each case.
	resultreg.Free(build);

	// For floating point results, we need to get a new register, since we can't
	// reuse the integer one used to store the random result.
	if (ValueType->GetRegType() == REGT_FLOAT)
	{
		resultreg = ExpEmit(build, REGT_FLOAT);
		resultreg.Free(build);
	}

	// Allocate space for the jump table.
	size_t jumptable = build->Emit(OP_JMP, 0);
	for (i = 1; i < choices.Size(); ++i)
	{
		build->Emit(OP_JMP, 0);
	}

	// Emit each case
	TArray<size_t> finishes(choices.Size() - 1);
	for (unsigned i = 0; i < choices.Size(); ++i)
	{
		build->BackpatchToHere(jumptable + i);
		if (choices[i]->isConstant())
		{
			EmitLoad(build, resultreg, static_cast<FxConstant *>(choices[i])->GetValue());
		}
		else
		{
			ExpEmit casereg = choices[i]->Emit(build);
			if (casereg.RegNum != resultreg.RegNum)
			{ // The result of the case is in a different register from what
			  // was expected. Copy it to the one we wanted.

				resultreg.Reuse(build);	// This is really just for the assert in Reuse()
				build->Emit(ValueType->GetRegType() == REGT_INT ? OP_MOVE : OP_MOVEF, resultreg.RegNum, casereg.RegNum, 0);
				resultreg.Free(build);
			}
			// Free this register so the remaining cases can use it.
			casereg.Free(build);
		}
		// All but the final case needs a jump to the end of the expression's code.
		if (i + 1 < choices.Size())
		{
			size_t loc = build->Emit(OP_JMP, 0);
			finishes.Push(loc);
		}
	}
	// Backpatch each case (except the last, since it ends here) to jump to here.
	for (i = 0; i < choices.Size() - 1; ++i)
	{
		build->BackpatchToHere(finishes[i]);
	}
	// The result register needs to be in-use when we return.
	// It should have been freed earlier, so restore its in-use flag.
	resultreg.Reuse(build);
	return resultreg;
}

//==========================================================================
//
//
//
//==========================================================================
FxFRandom::FxFRandom(FRandom *r, FxExpression *mi, FxExpression *ma, const FScriptPosition &pos)
: FxRandom(r, NULL, NULL, pos, true)
{
	if (mi != NULL && ma != NULL)
	{
		min = new FxFloatCast(mi);
		max = new FxFloatCast(ma);
	}
	ValueType = TypeFloat64;
}

//==========================================================================
//
//
//
//==========================================================================

int DecoFRandom(VMFrameStack *stack, VMValue *param, int numparam, VMReturn *ret, int numret)
{
	assert(numparam == 1 || numparam == 3);
	FRandom *rng = reinterpret_cast<FRandom *>(param[0].a);

	int random = (*rng)(0x40000000);
	double frandom = random / double(0x40000000);

	if (numparam == 3)
	{
		double min = param[1].f, max = param[2].f;
		if (max < min)
		{
			swapvalues(max, min);
		}
		ACTION_RETURN_FLOAT(frandom * (max - min) + min);
	}
	else
	{
		ACTION_RETURN_FLOAT(frandom);
	}
}

ExpEmit FxFRandom::Emit(VMFunctionBuilder *build)
{
	// Call the DecoFRandom function to generate a floating point random number..
	VMFunction *callfunc;
	PSymbol *sym = FindDecorateBuiltinFunction(NAME_DecoFRandom, DecoFRandom);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != NULL);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	int opcode = (EmitTail ? OP_TAIL_K : OP_CALL_K);

	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(rng, ATAG_RNG));
	if (min != NULL && max != NULL)
	{
		EmitParameter(build, min, ScriptPosition);
		EmitParameter(build, max, ScriptPosition);
		build->Emit(opcode, build->GetConstantAddress(callfunc, ATAG_OBJECT), 3, 1);
	}
	else
	{
		build->Emit(opcode, build->GetConstantAddress(callfunc, ATAG_OBJECT), 1, 1);
	}

	if (EmitTail)
	{
		ExpEmit call;
		call.Final = true;
		return call;
	}

	ExpEmit out(build, REGT_FLOAT);
	build->Emit(OP_RESULT, 0, REGT_FLOAT, out.RegNum);
	return out;
}

//==========================================================================
//
//
//
//==========================================================================

FxRandom2::FxRandom2(FRandom *r, FxExpression *m, const FScriptPosition &pos, bool nowarn)
: FxExpression(pos)
{
	EmitTail = false;
	rng = r;
	if (m) mask = new FxIntCast(m, nowarn);
	else mask = new FxConstant(-1, pos);
	ValueType = TypeSInt32;
}

//==========================================================================
//
//
//
//==========================================================================

FxRandom2::~FxRandom2()
{
	SAFE_DELETE(mask);
}

//==========================================================================
//
//
//
//==========================================================================

PPrototype *FxRandom2::ReturnProto()
{
	EmitTail = true;
	return FxExpression::ReturnProto();
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxRandom2::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(mask, ctx);
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxRandom2::Emit(VMFunctionBuilder *build)
{
	// Call the DecoRandom function to generate the random number.
	VMFunction *callfunc;
	PSymbol *sym = FindDecorateBuiltinFunction(NAME_DecoRandom, DecoRandom);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != NULL);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	int opcode = (EmitTail ? OP_TAIL_K : OP_CALL_K);

	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(rng, ATAG_RNG));
	EmitParameter(build, mask, ScriptPosition);
	build->Emit(opcode, build->GetConstantAddress(callfunc, ATAG_OBJECT), 2, 1);

	if (EmitTail)
	{
		ExpEmit call;
		call.Final = true;
		return call;
	}

	ExpEmit out(build, REGT_INT);
	build->Emit(OP_RESULT, 0, REGT_INT, out.RegNum);
	return out;
}

//==========================================================================
//
//
//
//==========================================================================

FxIdentifier::FxIdentifier(FName name, const FScriptPosition &pos)
: FxExpression(pos)
{
	Identifier = name;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxIdentifier::Resolve(FCompileContext& ctx)
{
	PSymbol * sym;
	FxExpression *newex = nullptr;
	int num;
	
	CHECKRESOLVED();

	// Local variables have highest priority.
	FxLocalVariableDeclaration *local = ctx.FindLocalVariable(Identifier);
	if (local != nullptr)
	{
		auto x = new FxLocalVariable(local, ScriptPosition);
		delete this;
		return x->Resolve(ctx);
	}
	// Ugh, the horror. Constants need to be taken from the owning class, but members from the self class to catch invalid accesses here...
	// see if the current class (if valid) defines something with this name.
	PSymbolTable *symtbl;
	if ((sym = ctx.FindInClass(Identifier, symtbl)) != nullptr)
	{
		if (sym->IsKindOf(RUNTIME_CLASS(PSymbolConst)))
		{
			ScriptPosition.Message(MSG_DEBUGLOG, "Resolving name '%s' as class constant\n", Identifier.GetChars());
			newex = FxConstant::MakeConstant(sym, ScriptPosition);
		}
		else if (sym->IsKindOf(RUNTIME_CLASS(PField)))
		{
			if (!ctx.Function)
			{
				ScriptPosition.Message(MSG_ERROR, "Cannot resolve class member outside a function", sym->SymbolName.GetChars());
				delete this;
				return nullptr;
			}
			PField *vsym = static_cast<PField*>(sym);

			// We have 4 cases to consider here:
			// 1. The symbol is a static/meta member (not implemented yet) which is always accessible.
			// 2. This is a static function 
			// 3. This is an action function with a restricted self pointer
			// 4. This is a normal member or unrestricted action function.
			if (vsym->Flags & VARF_Deprecated)
			{
				ScriptPosition.Message(MSG_WARNING, "Accessing deprecated member variable %s", sym->SymbolName.GetChars());
			}
			if ((vsym->Flags & VARF_Private) && symtbl != &ctx.Class->Symbols)
			{
				ScriptPosition.Message(MSG_ERROR, "Private member %s not accessible", sym->SymbolName.GetChars());
				delete this;
				return nullptr;
			}

			if (vsym->Flags & VARF_Static)
			{
				// todo. For now these cannot be defined so let's just exit.
				ScriptPosition.Message(MSG_ERROR, "Static members not implemented yet.");
				delete this;
				return nullptr;
			}

			if (ctx.Function->Variants[0].SelfClass == nullptr)
			{
				ScriptPosition.Message(MSG_ERROR, "Unable to access class member from static function");
				delete this;
				return nullptr;
			}

			if (ctx.Function->Variants[0].SelfClass != ctx.Class)
			{
				// Check if the restricted class can access it.
				PSymbol *sym2;
				if ((sym2 = ctx.FindInSelfClass(Identifier, symtbl)) != nullptr)
				{
					if (sym != sym2)
					{
						ScriptPosition.Message(MSG_ERROR, "Member variable of %s not accessible through restricted self pointer", ctx.Class->TypeName.GetChars());
						delete this;
						return nullptr;
					}
				}
			}
			ScriptPosition.Message(MSG_DEBUGLOG, "Resolving name '%s' as member variable, index %d\n", Identifier.GetChars(), vsym->Offset);
			newex = new FxClassMember((new FxSelf(ScriptPosition))->Resolve(ctx), vsym, ScriptPosition);
		}
		else
		{
			ScriptPosition.Message(MSG_ERROR, "Invalid member identifier '%s'\n", Identifier.GetChars());
			delete this;
			return nullptr;
		}
	}

	// now check the global identifiers.
	else if ((sym = ctx.FindGlobal(Identifier)) != NULL)
	{
		if (sym->IsKindOf(RUNTIME_CLASS(PSymbolConst)))
		{
			ScriptPosition.Message(MSG_DEBUGLOG, "Resolving name '%s' as global constant\n", Identifier.GetChars());
			newex = FxConstant::MakeConstant(sym, ScriptPosition);
		}
		else
		{
			ScriptPosition.Message(MSG_ERROR, "Invalid global identifier '%s'\n", Identifier.GetChars());
		}
	}

	// and line specials
	else if ((num = P_FindLineSpecial(Identifier, NULL, NULL)))
	{
		ScriptPosition.Message(MSG_DEBUGLOG, "Resolving name '%s' as line special %d\n", Identifier.GetChars(), num);
		newex = new FxConstant(num, ScriptPosition);
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Unknown identifier '%s'", Identifier.GetChars());
		delete this;
		return nullptr;
	}
	delete this;
	return newex? newex->Resolve(ctx) : nullptr;
}


//==========================================================================
//
//
//
//==========================================================================

FxMemberIdentifier::FxMemberIdentifier(FxExpression *left, FName name, const FScriptPosition &pos)
	: FxIdentifier(name, pos)
{
	Object = left;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxMemberIdentifier::Resolve(FCompileContext& ctx)
{
	PSymbol * sym;
	FxExpression *newex = nullptr;

	CHECKRESOLVED();

	SAFE_RESOLVE(Object, ctx);

	if (Object->ValueType->IsKindOf(RUNTIME_CLASS(PPointer)))
	{
		PSymbolTable *symtbl;
		auto ptype = static_cast<PPointer *>(Object->ValueType)->PointedType;

		if (ptype->IsKindOf(RUNTIME_CLASS(PClass)))
		{
			PClass *cls = static_cast<PClass *>(ptype);
			if ((sym = cls->Symbols.FindSymbolInTable(Identifier, symtbl)) != nullptr)
			{
				if (sym->IsKindOf(RUNTIME_CLASS(PSymbolConst)))
				{
					ScriptPosition.Message(MSG_DEBUGLOG, "Resolving name '%s' as class constant\n", Identifier.GetChars());
					newex = FxConstant::MakeConstant(sym, ScriptPosition);
				}
				else if (sym->IsKindOf(RUNTIME_CLASS(PField)))
				{
					PField *vsym = static_cast<PField*>(sym);

					// We have 4 cases to consider here:
					// 1. The symbol is a static/meta member (not implemented yet) which is always accessible.
					// 2. This is a static function 
					// 3. This is an action function with a restricted self pointer
					// 4. This is a normal member or unrestricted action function.
					if (vsym->Flags & VARF_Deprecated)
					{
						ScriptPosition.Message(MSG_WARNING, "Accessing deprecated member variable %s", vsym->SymbolName.GetChars());
					}
					if ((vsym->Flags & VARF_Private) && symtbl != &ctx.Class->Symbols)
					{
						ScriptPosition.Message(MSG_ERROR, "Private member %s not accessible", vsym->SymbolName.GetChars());
						delete this;
						return nullptr;
					}

					if (vsym->Flags & VARF_Static)
					{
						// todo. For now these cannot be defined so let's just exit.
						ScriptPosition.Message(MSG_ERROR, "Static members not implemented yet.");
						delete this;
						return nullptr;
					}
					auto x = new FxClassMember(Object, vsym, ScriptPosition);
					delete this;
					return x->Resolve(ctx);
				}
				else
				{
					ScriptPosition.Message(MSG_ERROR, "Invalid member identifier '%s'\n", Identifier.GetChars());
					delete this;
					return nullptr;
				}
			}
			else
			{
				ScriptPosition.Message(MSG_ERROR, "Unknown identifier '%s'", Identifier.GetChars());
				delete this;
				return nullptr;
			}
		}
		else if (ptype->IsA(RUNTIME_CLASS(PStruct)))
		{
			// todo
		}
	}
	else if (Object->ValueType->IsA(RUNTIME_CLASS(PStruct)))
	{
		// todo
	}

	ScriptPosition.Message(MSG_ERROR, "Left side of %s is not a struct or class", Identifier.GetChars());
	delete this;
	return nullptr;
}


//==========================================================================
//
//
//
//==========================================================================

FxLocalVariable::FxLocalVariable(FxLocalVariableDeclaration *var, const FScriptPosition &sc)
	: FxExpression(sc)
{
	Variable = var;
	ValueType = var->ValueType;
	AddressRequested = false;
}

FxExpression *FxLocalVariable::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	return this;
}

bool FxLocalVariable::RequestAddress(bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = !(Variable->VarFlags & VARF_ReadOnly);
	return true;
}
	
ExpEmit FxLocalVariable::Emit(VMFunctionBuilder *build)
{
	ExpEmit ret(Variable->RegNum, Variable->ValueType->GetRegType(), false, true);
	if (AddressRequested) ret.Target = true;
	return ret;
}


//==========================================================================
//
//
//
//==========================================================================

FxSelf::FxSelf(const FScriptPosition &pos)
: FxExpression(pos)
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxSelf::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	if (ctx.Function == nullptr || ctx.Function->Variants[0].SelfClass == nullptr)
	{
		ScriptPosition.Message(MSG_ERROR, "self used outside of a member function");
		delete this;
		return nullptr;
	}
	ValueType = NewPointer(ctx.Function->Variants[0].SelfClass);
	return this;
}  

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxSelf::Emit(VMFunctionBuilder *build)
{
	// self is always the first pointer passed to the function
	return ExpEmit(0, REGT_POINTER, false, true);
}


//==========================================================================
//
//
//
//==========================================================================

FxClassMember::FxClassMember(FxExpression *x, PField* mem, const FScriptPosition &pos)
: FxExpression(pos)
{
	classx = x;
	membervar = mem;
	AddressRequested = false;
	//if (classx->IsDefaultObject()) Readonly=true;
}

//==========================================================================
//
//
//
//==========================================================================

FxClassMember::~FxClassMember()
{
	SAFE_DELETE(classx);
}

//==========================================================================
//
//
//
//==========================================================================

bool FxClassMember::RequestAddress(bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = !(membervar->Flags & VARF_ReadOnly);
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxClassMember::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(classx, ctx);

	PPointer *ptrtype = dyn_cast<PPointer>(classx->ValueType);
	if (ptrtype == NULL || !ptrtype->IsKindOf(RUNTIME_CLASS(DObject)))
	{
		ScriptPosition.Message(MSG_ERROR, "Member variable requires a class or object");
		delete this;
		return NULL;
	}
	ValueType = membervar->Type;
	return this;
}

ExpEmit FxClassMember::Emit(VMFunctionBuilder *build)
{
	ExpEmit obj = classx->Emit(build);
	assert(obj.RegType == REGT_POINTER);

	if (obj.Konst)
	{
		// If the situation where we are dereferencing a constant
		// pointer is common, then it would probably be worthwhile
		// to add new opcodes for those. But as of right now, I
		// don't expect it to be a particularly common case.
		ExpEmit newobj(build, REGT_POINTER);
		build->Emit(OP_LKP, newobj.RegNum, obj.RegNum);
		obj = newobj;
	}

	if (AddressRequested)
	{
		if (membervar->Offset == 0)
		{
			return obj;
		}
		obj.Free(build);
		ExpEmit out(build, REGT_POINTER);
		build->Emit(OP_ADDA_RK, out.RegNum, obj.RegNum, build->GetConstantInt((int)membervar->Offset));
		return out;
	}

	int offsetreg = build->GetConstantInt((int)membervar->Offset);
	ExpEmit loc(build, membervar->Type->GetRegType());

	build->Emit(membervar->Type->GetLoadOp(), loc.RegNum, obj.RegNum, offsetreg);
	obj.Free(build);
	return loc;
}


//==========================================================================
//
//
//
//==========================================================================

FxArrayElement::FxArrayElement(FxExpression *base, FxExpression *_index)
:FxExpression(base->ScriptPosition)
{
	Array=base;
	index = _index;
	AddressRequested = false;
	AddressWritable = false;
}

//==========================================================================
//
//
//
//==========================================================================

FxArrayElement::~FxArrayElement()
{
	SAFE_DELETE(Array);
	SAFE_DELETE(index);
}

//==========================================================================
//
//
//
//==========================================================================

bool FxArrayElement::RequestAddress(bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = AddressWritable;
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxArrayElement::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Array,ctx);
	SAFE_RESOLVE(index,ctx);

	if (index->ValueType->GetRegType() == REGT_FLOAT /* lax */)
	{
		// DECORATE allows floats here so cast them to int.
		index = new FxIntCast(index, ctx.FromDecorate);
		index = index->Resolve(ctx);
		if (index == NULL) 
		{
			delete this;
			return NULL;
		}
	}
	if (index->ValueType->GetRegType() != REGT_INT && index->ValueType != TypeName)
	{
		ScriptPosition.Message(MSG_ERROR, "Array index must be integer");
		delete this;
		return NULL;
	}

	PArray *arraytype = dyn_cast<PArray>(Array->ValueType);
	if (arraytype == NULL)
	{
		ScriptPosition.Message(MSG_ERROR, "'[]' can only be used with arrays.");
		delete this;
		return NULL;
	}
	if (index->isConstant())
	{
		unsigned indexval = static_cast<FxConstant *>(index)->GetValue().GetInt();
		if (indexval >= arraytype->ElementCount)
		{
			ScriptPosition.Message(MSG_ERROR, "Array index out of bounds");
			delete this;
			return NULL;
		}
	}

	ValueType = arraytype->ElementType;
	if (ValueType->GetRegType() != REGT_INT && ValueType->GetRegType() != REGT_FLOAT)
	{
		// int arrays only for now
		ScriptPosition.Message(MSG_ERROR, "Only numeric arrays are supported.");
		delete this;
		return NULL;
	}
	if (!Array->RequestAddress(&AddressWritable))
	{
		ScriptPosition.Message(MSG_ERROR, "Unable to dereference array.");
		delete this;
		return NULL;
	}
	return this;
}

//==========================================================================
//
// in its current state this won't be able to do more than handle the args array.
//
//==========================================================================

ExpEmit FxArrayElement::Emit(VMFunctionBuilder *build)
{
	ExpEmit start = Array->Emit(build);
	PArray *const arraytype = static_cast<PArray*>(Array->ValueType);
	ExpEmit dest(build, arraytype->ElementType->GetRegType());

	if (start.Konst)
	{
		ExpEmit tmpstart(build, REGT_POINTER);
		build->Emit(OP_LKP, tmpstart.RegNum, start.RegNum);
		start.Free(build);
		start = tmpstart;
	}
	if (index->isConstant())
	{
		unsigned indexval = static_cast<FxConstant *>(index)->GetValue().GetInt();
		assert(indexval < arraytype->ElementCount && "Array index out of bounds");
		indexval *= arraytype->ElementSize;

		if (AddressRequested)
		{
			if (indexval != 0)
			{
				build->Emit(OP_ADDA_RK, start.RegNum, start.RegNum, build->GetConstantInt(indexval));
			}
		}
		else
		{
			build->Emit(arraytype->ElementType->GetLoadOp(), dest.RegNum,
				start.RegNum, build->GetConstantInt(indexval));
		}
	}
	else
	{
		ExpEmit indexv(index->Emit(build));
		int shiftbits = 0;
		while (1u << shiftbits < arraytype->ElementSize)
		{ 
			shiftbits++;
		}
		assert(1u << shiftbits == arraytype->ElementSize && "Element sizes other than power of 2 are not implemented");
		build->Emit(OP_BOUND, indexv.RegNum, arraytype->ElementCount);
		if (shiftbits > 0)
		{
			build->Emit(OP_SLL_RI, indexv.RegNum, indexv.RegNum, shiftbits);
		}

		if (AddressRequested)
		{
			build->Emit(OP_ADDA_RR, start.RegNum, start.RegNum, indexv.RegNum);
		}
		else
		{
			build->Emit(arraytype->ElementType->GetLoadOp() + 1,	// added 1 to use the *_R version that
				dest.RegNum, start.RegNum, indexv.RegNum);			// takes the offset from a register
		}
		indexv.Free(build);
	}
	if (AddressRequested)
	{
		dest.Free(build);
		return start;
	}
	
	start.Free(build);
	return dest;
}

//==========================================================================
//
//
//
//==========================================================================

FxFunctionCall::FxFunctionCall(FName methodname, FName rngname, FArgumentList *args, const FScriptPosition &pos)
: FxExpression(pos)
{
	MethodName = methodname;
	RNG = nullptr;
	ArgList = args;
	if (rngname != NAME_None)
	{
		switch (MethodName)
		{
		case NAME_Random:
		case NAME_FRandom:
		case NAME_RandomPick:
		case NAME_FRandomPick:
		case NAME_Random2:
			RNG = FRandom::StaticFindRNG(rngname.GetChars());
			break;

		default:
			pos.Message(MSG_ERROR, "Cannot use named RNGs with %s", MethodName.GetChars());
			break;

		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxFunctionCall::~FxFunctionCall()
{
	SAFE_DELETE(ArgList);
}

//==========================================================================
//
// Check function that gets called
//
//==========================================================================

static bool CheckArgSize(FName fname, FArgumentList *args, int min, int max, FScriptPosition &sc)
{
	int s = args ? args->Size() : 0;
	if (s < min)
	{
		sc.Message(MSG_ERROR, "Insufficient arguments in call to %s, expected %d, got %d", fname.GetChars(), min, s);
		return false;
	}
	else if (s > max && max >= 0)
	{
		sc.Message(MSG_ERROR, "Too many arguments in call to %s, expected %d, got %d", fname.GetChars(), min, s);
		return false;
	}
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxFunctionCall::Resolve(FCompileContext& ctx)
{
	ABORT(ctx.Class);
	bool error = false;

	PFunction *afd = FindClassMemberFunction(ctx.Class, ctx.Class, MethodName, ScriptPosition, &error);

	if (error)
	{
		delete this;
		return nullptr;
	}

	if (afd != nullptr)
	{
		if (ctx.Function->Variants[0].Flags & VARF_Static && !(afd->Variants[0].Flags & VARF_Static))
		{
			ScriptPosition.Message(MSG_ERROR, "Call to non-static function %s from a static context", MethodName.GetChars());
			delete this;
			return nullptr;
		}
		auto self = !(afd->Variants[0].Flags & VARF_Static)? new FxSelf(ScriptPosition) : nullptr;
		auto x = new FxVMFunctionCall(self, afd, ArgList, ScriptPosition, false);
		ArgList = nullptr;
		delete this;
		return x->Resolve(ctx);
	}

	for (size_t i = 0; i < countof(FxFlops); ++i)
	{
		if (MethodName == FxFlops[i].Name)
		{
			FxExpression *x = new FxFlopFunctionCall(i, ArgList, ScriptPosition);
			ArgList = nullptr;
			delete this;
			return x->Resolve(ctx);
		}
	}

	int min, max, special;
	if (MethodName == NAME_ACS_NamedExecuteWithResult || MethodName == NAME_CallACS)
	{
		special = -ACS_ExecuteWithResult;
		min = 1;
		max = 5;
	}
	else
	{
		special = P_FindLineSpecial(MethodName.GetChars(), &min, &max);
	}
	if (special != 0 && min >= 0)
	{
		int paramcount = ArgList? ArgList->Size() : 0;
		if (paramcount < min)
		{
			ScriptPosition.Message(MSG_ERROR, "Not enough parameters for '%s' (expected %d, got %d)", 
				MethodName.GetChars(), min, paramcount);
			delete this;
			return nullptr;
		}
		else if (paramcount > max)
		{
			ScriptPosition.Message(MSG_ERROR, "too many parameters for '%s' (expected %d, got %d)", 
				MethodName.GetChars(), max, paramcount);
			delete this;
			return nullptr;
		}
		FxExpression *self = (ctx.Function && ctx.Function->Variants[0].Flags & VARF_Method) ? new FxSelf(ScriptPosition) : nullptr;
		FxExpression *x = new FxActionSpecialCall(self, special, ArgList, ScriptPosition);
		ArgList = nullptr;
		delete this;
		return x->Resolve(ctx);
	}

	// Last but not least: Check builtins. The random functions can take a named RNG if specified.
	// Note that for all builtins the used arguments have to be nulled in the ArgList so that they won't get deleted before they get used.
	FxExpression *func = nullptr;

	switch (MethodName)
	{
	case NAME_Random:
		if (CheckArgSize(NAME_Random, ArgList, 2, 2, ScriptPosition))
		{
			func = new FxRandom(RNG, (*ArgList)[0], (*ArgList)[1], ScriptPosition, ctx.FromDecorate);
			(*ArgList)[0] = (*ArgList)[1] = nullptr;
		}
		break;

	case NAME_FRandom:
		if (CheckArgSize(NAME_FRandom, ArgList, 2, 2, ScriptPosition))
		{
			func = new FxFRandom(RNG, (*ArgList)[0], (*ArgList)[1], ScriptPosition);
			(*ArgList)[0] = (*ArgList)[1] = nullptr;
		}
		break;

	case NAME_RandomPick:
	case NAME_FRandomPick:
		if (CheckArgSize(MethodName, ArgList, 1, -1, ScriptPosition))
		{
			func = new FxRandomPick(RNG, *ArgList, MethodName == NAME_FRandomPick, ScriptPosition, ctx.FromDecorate);
			for (auto &i : *ArgList) i = nullptr;
		}
		break;

	case NAME_Random2:
		if (CheckArgSize(NAME_Random2, ArgList, 0, 1, ScriptPosition))
		{
			func = new FxRandom2(RNG, ArgList->Size() == 0? nullptr : (*ArgList)[0], ScriptPosition, ctx.FromDecorate);
			if (ArgList->Size() > 0) (*ArgList)[0] = nullptr;
		}
		break;

	case NAME_Min:
	case NAME_Max:
		if (CheckArgSize(MethodName, ArgList, 2, -1, ScriptPosition))
		{
			func = new FxMinMax(*ArgList, MethodName, ScriptPosition);
			for (auto &i : *ArgList) i = nullptr;
		}
		break;

	case NAME_Clamp:
		if (CheckArgSize(MethodName, ArgList, 3, 3, ScriptPosition))
		{
			TArray<FxExpression *> pass;
			pass.Resize(2);
			pass[0] = (*ArgList)[0];
			pass[1] = (*ArgList)[1];
			pass[0] = new FxMinMax(pass, NAME_Max, ScriptPosition);
			pass[1] = (*ArgList)[2];
			func = new FxMinMax(*ArgList, NAME_Min, ScriptPosition);
			(*ArgList)[0] = (*ArgList)[1] = (*ArgList)[2] = nullptr;
		}
		break;

	case NAME_Abs:
		if (CheckArgSize(MethodName, ArgList, 1, 1, ScriptPosition))
		{
			func = new FxAbs((*ArgList)[0]);
			(*ArgList)[0] = nullptr;
		}
		break;

	case NAME_ATan2:
	case NAME_VectorAngle:
		if (CheckArgSize(MethodName, ArgList, 2, 2, ScriptPosition))
		{
			func = MethodName == NAME_ATan2 ? new FxATan2((*ArgList)[0], (*ArgList)[1], ScriptPosition) : new FxATan2((*ArgList)[1], (*ArgList)[0], ScriptPosition);
			(*ArgList)[0] = (*ArgList)[1] = nullptr;
		}
		break;

	default:
		break;
	}
	if (func != nullptr)
	{
		delete this;
		return func->Resolve(ctx);
	}
	ScriptPosition.Message(MSG_ERROR, "Call to unknown function '%s'", MethodName.GetChars());
	delete this;
	return nullptr;
}


//==========================================================================
//
//
//
//==========================================================================

FxMemberFunctionCall::FxMemberFunctionCall(FxExpression *self, FName methodname, FArgumentList *args, const FScriptPosition &pos)
	: FxExpression(pos)
{
	Self = self;
	MethodName = methodname;
	ArgList = args;
}

//==========================================================================
//
//
//
//==========================================================================

FxMemberFunctionCall::~FxMemberFunctionCall()
{
	SAFE_DELETE(Self);
	SAFE_DELETE(ArgList);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxMemberFunctionCall::Resolve(FCompileContext& ctx)
{
	ABORT(ctx.Class);
	SAFE_RESOLVE(Self, ctx);

	PClass *cls;
	bool staticonly = false;
	if (Self->ValueType->IsKindOf(RUNTIME_CLASS(PClassPointer)))
	{
		cls = static_cast<PClassPointer *>(Self->ValueType)->ClassRestriction;
		staticonly = true;
	}
	else if (Self->ValueType->IsKindOf(RUNTIME_CLASS(PPointer)))
	{
		auto ptype = static_cast<PPointer *>(Self->ValueType)->PointedType;
		if (ptype->IsKindOf(RUNTIME_CLASS(PClass)))
		{
			cls = static_cast<PClass *>(ptype);
		}
		else
		{
			ScriptPosition.Message(MSG_ERROR, "Left hand side of %s must point to a class object\n", MethodName.GetChars());
			delete this;
			return nullptr;
		}
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Invalid expression on left hand side of %s\n", MethodName.GetChars());
		delete this;
		return nullptr;
	}

	bool error = false;
	PFunction *afd = FindClassMemberFunction(cls, cls, MethodName, ScriptPosition, &error);
	if (error)
	{
		delete this;
		return nullptr;
	}

	if (afd == nullptr)
	{
		ScriptPosition.Message(MSG_ERROR, "Unknown function %s\n", MethodName.GetChars());
		delete this;
		return nullptr;
	}
	if (staticonly && !(afd->Variants[0].Flags & VARF_Static))
	{
		if (!ctx.Class->IsDescendantOf(cls))
		{
			ScriptPosition.Message(MSG_ERROR, "Cannot call non-static function %s::%s from here\n", cls->TypeName.GetChars(), MethodName.GetChars());
			delete this;
			return nullptr;
		}
		// If this is a qualified call to a parent class function, let it through (but this needs to disable virtual calls later.)
	}

	// do not pass the self pointer to static functions.
	auto self = !(afd->Variants[0].Flags & VARF_Static) ? Self : nullptr;
	auto x = new FxVMFunctionCall(self, afd, ArgList, ScriptPosition, staticonly);
	ArgList = nullptr;
	delete this;
	return x->Resolve(ctx);
}


//==========================================================================
//
// FxActionSpecialCall
//
// If special is negative, then the first argument will be treated as a
// name for ACS_NamedExecuteWithResult.
//
//==========================================================================

FxActionSpecialCall::FxActionSpecialCall(FxExpression *self, int special, FArgumentList *args, const FScriptPosition &pos)
: FxExpression(pos)
{
	Self = self;
	Special = special;
	ArgList = args;
	EmitTail = false;
}

//==========================================================================
//
//
//
//==========================================================================

FxActionSpecialCall::~FxActionSpecialCall()
{
	SAFE_DELETE(Self);
	SAFE_DELETE(ArgList);
}

//==========================================================================
//
//
//
//==========================================================================

PPrototype *FxActionSpecialCall::ReturnProto()
{
	EmitTail = true;
	return FxExpression::ReturnProto();
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxActionSpecialCall::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	bool failed = false;

	SAFE_RESOLVE_OPT(Self, ctx);
	if (ArgList != NULL)
	{
		for (unsigned i = 0; i < ArgList->Size(); i++)
		{
			(*ArgList)[i] = (*ArgList)[i]->Resolve(ctx);
			if ((*ArgList)[i] == NULL) failed = true;
			if (Special < 0 && i == 0)
			{
				if ((*ArgList)[i]->ValueType != TypeName)
				{
					ScriptPosition.Message(MSG_ERROR, "Name expected for parameter %d", i);
					failed = true;
				}
			}
			else if ((*ArgList)[i]->ValueType->GetRegType() != REGT_INT)
			{
				if ((*ArgList)[i]->ValueType->GetRegType() == REGT_FLOAT /* lax */)
				{
					(*ArgList)[i] = new FxIntCast((*ArgList)[i], ctx.FromDecorate);
				}
				else
				{
					ScriptPosition.Message(MSG_ERROR, "Integer expected for parameter %d", i);
					failed = true;
				}
			}
		}
		if (failed)
		{
			delete this;
			return NULL;
		}
	}
	ValueType = TypeSInt32;
	return this;
}


//==========================================================================
//
// 
//
//==========================================================================

int DecoCallLineSpecial(VMFrameStack *stack, VMValue *param, int numparam, VMReturn *ret, int numret)
{
	assert(numparam > 2 && numparam < 8);
	assert(param[0].Type == REGT_INT);
	assert(param[1].Type == REGT_POINTER);
	int v[5] = { 0 };

	for (int i = 2; i < numparam; ++i)
	{
		v[i - 2] = param[i].i;
	}
	ACTION_RETURN_INT(P_ExecuteSpecial(param[0].i, NULL, reinterpret_cast<AActor*>(param[1].a), false, v[0], v[1], v[2], v[3], v[4]));
}

ExpEmit FxActionSpecialCall::Emit(VMFunctionBuilder *build)
{
	unsigned i = 0;

	build->Emit(OP_PARAMI, abs(Special));			// pass special number
	// fixme: This really should use the Self pointer that got passed to this class instead of just using the first argument from the function. 
	// Once static functions are possible, or specials can be called through a member access operator this won't work anymore.
	build->Emit(OP_PARAM, 0, REGT_POINTER, 0);		// pass self 
	if (ArgList != NULL)
	{
		for (; i < ArgList->Size(); ++i)
		{
			FxExpression *argex = (*ArgList)[i];
			if (Special < 0 && i == 0)
			{
				assert(argex->ValueType == TypeName);
				assert(argex->isConstant());
				build->EmitParamInt(-static_cast<FxConstant *>(argex)->GetValue().GetName());
			}
			else
			{
				assert(argex->ValueType->GetRegType() == REGT_INT);
				if (argex->isConstant())
				{
					build->EmitParamInt(static_cast<FxConstant *>(argex)->GetValue().GetInt());
				}
				else
				{
					ExpEmit arg(argex->Emit(build));
					build->Emit(OP_PARAM, 0, arg.RegType, arg.RegNum);
					arg.Free(build);
				}
			}
		}
	}
	// Call the DecoCallLineSpecial function to perform the desired special.
	VMFunction *callfunc;
	PSymbol *sym = FindDecorateBuiltinFunction(NAME_DecoCallLineSpecial, DecoCallLineSpecial);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != NULL);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	if (EmitTail)
	{
		build->Emit(OP_TAIL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 2 + i, 0);
		ExpEmit call;
		call.Final = true;
		return call;
	}

	ExpEmit dest(build, REGT_INT);
	build->Emit(OP_CALL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 2 + i, 1);
	build->Emit(OP_RESULT, 0, REGT_INT, dest.RegNum);
	return dest;
}

//==========================================================================
//
// FxVMFunctionCall
//
//==========================================================================

FxVMFunctionCall::FxVMFunctionCall(FxExpression *self, PFunction *func, FArgumentList *args, const FScriptPosition &pos, bool novirtual)
: FxExpression(pos)
{
	Self = self;
	Function = func;
	ArgList = args;
	EmitTail = false;
	NoVirtual = novirtual;
}

//==========================================================================
//
//
//
//==========================================================================

FxVMFunctionCall::~FxVMFunctionCall()
{
	SAFE_DELETE(ArgList);
}

//==========================================================================
//
//
//
//==========================================================================

PPrototype *FxVMFunctionCall::ReturnProto()
{
	EmitTail = true;
	return Function->Variants[0].Proto;
}

//==========================================================================
//
//
//
//==========================================================================

VMFunction *FxVMFunctionCall::GetDirectFunction()
{
	// If this return statement calls a function with no arguments,
	// then it can be a "direct" function. That is, the DECORATE
	// definition can call that function directly without wrapping
	// it inside VM code.
	if ((ArgList ? ArgList->Size() : 0) == 0 && (Function->Variants[0].Flags & VARF_Action))
	{
		return Function->Variants[0].Implementation;
	}
	
	return nullptr;
}

//==========================================================================
//
// FxVMFunctionCall :: Resolve
//
//==========================================================================

FxExpression *FxVMFunctionCall::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE_OPT(Self, ctx);
	bool failed = false;
	auto proto = Function->Variants[0].Proto;
	auto argtypes = proto->ArgumentTypes;

	int implicit;
	if (Function->Variants[0].Flags & VARF_Action) implicit = 3;
	else if (Function->Variants[0].Flags & VARF_Method) implicit = 1;
	else implicit = 0;

	// This should never happen.
	if (Self == nullptr && !(Function->Variants[0].Flags & VARF_Static))
	{
		ScriptPosition.Message(MSG_ERROR, "Call to non-static function without a self pointer");
		delete this;
		return nullptr;
	}

	if (ArgList != NULL)
	{
		for (unsigned i = 0; i < ArgList->Size(); i++)
		{
			FxExpression *x = new FxTypeCast((*ArgList)[i], argtypes[i + implicit], false);
			x = x->Resolve(ctx);
			failed |= (x == nullptr);
			(*ArgList)[i] = x;
		}
	}
	if (failed)
	{
		delete this;
		return NULL;
	}
	TArray<PType *> &rets = proto->ReturnTypes;
	if (rets.Size() > 0)
	{
		ValueType = rets[0];
	}
	else
	{
		ValueType = TypeVoid;
	}

	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxVMFunctionCall::Emit(VMFunctionBuilder *build)
{
	assert((build->IsActionFunc && build->Registers[REGT_POINTER].GetMostUsed() >= NAP) ||
		(!build->IsActionFunc && build->Registers[REGT_POINTER].GetMostUsed() >= 1));
	int count = (ArgList ? ArgList->Size() : 0);

	if (count == 1)
	{
		ExpEmit reg;
		if (CheckEmitCast(build, EmitTail, reg))
		{
			return reg;
		}
	}

	// If both functions are non-action or both are action, there is no need for special treatment.
	//if (elf || (!!(Function->Variants[0].Flags & VARF_Action) == build->IsActionFunc))
	{
		// Emit code to pass implied parameters
		if (Function->Variants[0].Flags & VARF_Method)
		{
			assert(Self != nullptr);
			Self->Emit(build);
			count += 1;
		}
		if (Function->Variants[0].Flags & VARF_Action)
		{
			static_assert(NAP == 3, "This code needs to be updated if NAP changes");
			if (build->IsActionFunc)
			{
				build->Emit(OP_PARAM, 0, REGT_POINTER, 1);
				build->Emit(OP_PARAM, 0, REGT_POINTER, 2);
			}
			else
			{
				int null = build->GetConstantAddress(nullptr, ATAG_GENERIC);
				build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, null);
				build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, null);
			}
			count += 2;
		}
	}
	/*
	else
	{
		if (build->IsActionFunc && (Function->Variants[0].Flags & VARF_Method))
		{
			build->Emit(OP_PARAM, 0, REGT_POINTER, 0);
			count += 1;
		}
		// and calling an action function from a non-action function.
		// This must be blocked because it lacks crucial information.
		if (!build->IsActionFunc && (Function->Variants[0].Flags & VARF_Action))
		{
			// This case should be eliminated in the analyzing stage.
			I_Error("Cannot call action function from non-action functions.");
		}
	}
	*/

	// Emit code to pass explicit parameters
	if (ArgList != NULL)
	{
		for (unsigned i = 0; i < ArgList->Size(); ++i)
		{
			EmitParameter(build, (*ArgList)[i], ScriptPosition);
		}
	}
	// Get a constant register for this function
	VMFunction *vmfunc = Function->Variants[0].Implementation;
	int funcaddr = build->GetConstantAddress(vmfunc, ATAG_OBJECT);
	// Emit the call
	if (EmitTail)
	{ // Tail call
		build->Emit(OP_TAIL_K, funcaddr, count, 0);
		ExpEmit call;
		call.Final = true;
		return call;
	}
	else if (vmfunc->Proto->ReturnTypes.Size() > 0)
	{ // Call, expecting one result
		ExpEmit reg(build, vmfunc->Proto->ReturnTypes[0]->GetRegType());
		build->Emit(OP_CALL_K, funcaddr, count, 1);
		build->Emit(OP_RESULT, 0, reg.RegType, reg.RegNum);
		return reg;
	}
	else
	{ // Call, expecting no results
		build->Emit(OP_CALL_K, funcaddr, count, 0);
		return ExpEmit();
	}
}

//==========================================================================
//
// If calling one of the casting kludge functions, don't bother calling the
// function; just use the parameter directly. Returns true if this was a
// kludge function, false otherwise.
//
//==========================================================================

bool FxVMFunctionCall::CheckEmitCast(VMFunctionBuilder *build, bool returnit, ExpEmit &reg)
{
	FName funcname = Function->SymbolName;
	if (funcname == NAME___decorate_internal_int__ ||
		funcname == NAME___decorate_internal_bool__ ||
		funcname == NAME___decorate_internal_state__ ||
		funcname == NAME___decorate_internal_float__)
	{
		FxExpression *arg = (*ArgList)[0];
		if (returnit)
		{
			if (arg->isConstant() &&
				(funcname == NAME___decorate_internal_int__ ||
				 funcname == NAME___decorate_internal_bool__))
			{ // Use immediate version for integers in range
				build->EmitRetInt(0, true, static_cast<FxConstant *>(arg)->GetValue().Int);
			}
			else
			{
				ExpEmit where = arg->Emit(build);
				build->Emit(OP_RET, RET_FINAL, where.RegType | (where.Konst ? REGT_KONST : 0), where.RegNum);
				where.Free(build);
			}
			reg = ExpEmit();
			reg.Final = true;
		}
		else
		{
			reg = arg->Emit(build);
		}
		return true;
	}
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

FxFlopFunctionCall::FxFlopFunctionCall(size_t index, FArgumentList *args, const FScriptPosition &pos)
: FxExpression(pos)
{
	assert(index < countof(FxFlops) && "FLOP index out of range");
	Index = (int)index;
	ArgList = args;
}

//==========================================================================
//
//
//
//==========================================================================

FxFlopFunctionCall::~FxFlopFunctionCall()
{
	SAFE_DELETE(ArgList);
}

FxExpression *FxFlopFunctionCall::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	if (ArgList == NULL || ArgList->Size() != 1)
	{
		ScriptPosition.Message(MSG_ERROR, "%s only has one parameter", FName(FxFlops[Index].Name).GetChars());
		delete this;
		return NULL;
	}

	(*ArgList)[0] = (*ArgList)[0]->Resolve(ctx);
	if ((*ArgList)[0] == NULL)
	{
		delete this;
		return NULL;
	}

	if (!(*ArgList)[0]->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "numeric value expected for parameter");
		delete this;
		return NULL;
	}
	if ((*ArgList)[0]->isConstant())
	{
		double v = static_cast<FxConstant *>((*ArgList)[0])->GetValue().GetFloat();
		v = FxFlops[Index].Evaluate(v);
		FxExpression *x = new FxConstant(v, ScriptPosition);
		delete this;
		return x;
	}
	if ((*ArgList)[0]->ValueType->GetRegType() == REGT_INT)
	{
		(*ArgList)[0] = new FxFloatCast((*ArgList)[0]);
	}
	ValueType = TypeFloat64;
	return this;
}

//==========================================================================
//
//
//==========================================================================

ExpEmit FxFlopFunctionCall::Emit(VMFunctionBuilder *build)
{
	ExpEmit v = (*ArgList)[0]->Emit(build);
	assert(!v.Konst && v.RegType == REGT_FLOAT);

	build->Emit(OP_FLOP, v.RegNum, v.RegNum, FxFlops[Index].Flop);
	return v;
}

//==========================================================================
//
// FxSequence :: Resolve
//
//==========================================================================

FxExpression *FxSequence::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	for (unsigned i = 0; i < Expressions.Size(); ++i)
	{
		if (NULL == (Expressions[i] = Expressions[i]->Resolve(ctx)))
		{
			delete this;
			return nullptr;
		}
	}
	return this;
}

//==========================================================================
//
// FxSequence :: Emit
//
//==========================================================================

ExpEmit FxSequence::Emit(VMFunctionBuilder *build)
{
	for (unsigned i = 0; i < Expressions.Size(); ++i)
	{
		ExpEmit v = Expressions[i]->Emit(build);
		// Throw away any result. We don't care about it.
		v.Free(build);
	}
	return ExpEmit();
}

//==========================================================================
//
// FxSequence :: GetDirectFunction
//
//==========================================================================

VMFunction *FxSequence::GetDirectFunction()
{
	if (Expressions.Size() == 1)
	{
		return Expressions[0]->GetDirectFunction();
	}
	return NULL;
}

//==========================================================================
//
// FxCompoundStatement :: Resolve
//
//==========================================================================

FxExpression *FxCompoundStatement::Resolve(FCompileContext &ctx)
{
	auto outer = ctx.Block;
	Outer = ctx.Block;
	ctx.Block = this;
	auto x = FxSequence::Resolve(ctx);
	ctx.Block = outer;
	return x;
}

//==========================================================================
//
// FxCompoundStatement :: Emit
//
//==========================================================================

ExpEmit FxCompoundStatement::Emit(VMFunctionBuilder *build)
{
	auto e = FxSequence::Emit(build);
	// Release all local variables in this block.
	for (auto l : LocalVars)
	{
		l->Release(build);
	}
	return e;
}

//==========================================================================
//
// FxCompoundStatement :: FindLocalVariable
//
// Looks for a variable name in any of the containing compound statements
// This does a simple linear search on each block's variables. 
// The lists here normally don't get large enough to justify something more complex.
//
//==========================================================================

FxLocalVariableDeclaration *FxCompoundStatement::FindLocalVariable(FName name, FCompileContext &ctx)
{
	auto block = this;
	while (block != nullptr)
	{
		for (auto l : block->LocalVars)
		{
			if (l->Name == name)
			{
				return l;
			}
		}
		block = block->Outer;
	}
	// finally check the context for function arguments
	for (auto arg : ctx.FunctionArgs)
	{
		if (arg->Name == name)
		{
			return arg;
		}
	}
	return nullptr;
}

//==========================================================================
//
// FxCompoundStatement :: CheckLocalVariable
//
// Checks if the current block already contains a local variable 
// of the given name.
//
//==========================================================================

bool FxCompoundStatement::CheckLocalVariable(FName name)
{
	for (auto l : LocalVars)
	{
		if (l->Name == name)
		{
			return true;
		}
	}
	return false;
}

//==========================================================================
//
// FxIfStatement
//
//==========================================================================

FxIfStatement::FxIfStatement(FxExpression *cond, FxExpression *true_part,
	FxExpression *false_part, const FScriptPosition &pos)
: FxExpression(pos)
{
	Condition = cond;
	WhenTrue = true_part;
	WhenFalse = false_part;
	assert(cond != NULL);
}

FxIfStatement::~FxIfStatement()
{
	SAFE_DELETE(Condition);
	SAFE_DELETE(WhenTrue);
	SAFE_DELETE(WhenFalse);
}

FxExpression *FxIfStatement::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();

	if (WhenTrue == nullptr && WhenFalse == nullptr)
	{ // We don't do anything either way, so disappear
		delete this;
		return nullptr;
	}

	SAFE_RESOLVE(Condition, ctx);

	if (Condition->ValueType != TypeBool)
	{
		Condition = new FxBoolCast(Condition);
		SAFE_RESOLVE(Condition, ctx);
	}

	if (WhenTrue != nullptr)
	{
		WhenTrue = WhenTrue->Resolve(ctx);
		ABORT(WhenTrue);
	}
	if (WhenFalse != nullptr)
	{
		WhenFalse = WhenFalse->Resolve(ctx);
		ABORT(WhenFalse);
	}

	ValueType = TypeVoid;

	if (Condition->isConstant())
	{
		ExpVal condval = static_cast<FxConstant *>(Condition)->GetValue();
		bool result = condval.GetBool();

		FxExpression *e = result ? WhenTrue : WhenFalse;
		delete (result ? WhenFalse : WhenTrue);
		WhenTrue = WhenFalse = NULL;
		if (e == NULL) e = new FxNop(ScriptPosition);	// create a dummy if this statement gets completely removed by optimizing out the constant parts.
		delete this;
		return e;
	}

	return this;
}

ExpEmit FxIfStatement::Emit(VMFunctionBuilder *build)
{
	ExpEmit v;
	size_t jumpspot;
	FxExpression *path1, *path2;
	int condcheck;

	// This is pretty much copied from FxConditional, except we don't
	// keep any results.
	ExpEmit cond = Condition->Emit(build);
	assert(cond.RegType == REGT_INT && !cond.Konst);

	if (WhenTrue != NULL)
	{
		path1 = WhenTrue;
		path2 = WhenFalse;
		condcheck = 1;
	}
	else
	{
		// When there is only a false path, reverse the condition so we can
		// treat it as a true path.
		assert(WhenFalse != NULL);
		path1 = WhenFalse;
		path2 = NULL;
		condcheck = 0;
	}

	// Test condition.
	build->Emit(OP_EQ_K, condcheck, cond.RegNum, build->GetConstantInt(0));
	jumpspot = build->Emit(OP_JMP, 0);
	cond.Free(build);

	// Evaluate first path
	v = path1->Emit(build);
	v.Free(build);
	if (path2 != NULL)
	{
		size_t path1jump = build->Emit(OP_JMP, 0);
		// Evaluate second path
		build->BackpatchToHere(jumpspot);
		v = path2->Emit(build);
		v.Free(build);
		jumpspot = path1jump;
	}
	build->BackpatchToHere(jumpspot);
	return ExpEmit();
}

//==========================================================================
//
// FxLoopStatement :: Resolve
//
// saves the loop pointer in the context and sets this object as the current loop
// so that continues and breaks always resolve to the innermost loop.
//
//==========================================================================

FxExpression *FxLoopStatement::Resolve(FCompileContext &ctx)
{
	auto outer = ctx.Loop;
	ctx.Loop = this;
	auto x = DoResolve(ctx);
	ctx.Loop = outer;
	return x;
}

void FxLoopStatement::Backpatch(VMFunctionBuilder *build, size_t loopstart, size_t loopend)
{
	// Give a proper address to any break/continue statement within this loop.
	for (unsigned int i = 0; i < Jumps.Size(); i++)
	{
		if (Jumps[i]->Token == TK_Break)
		{
			build->Backpatch(Jumps[i]->Address, loopend);
		}
		else
		{ // Continue statement.
			build->Backpatch(Jumps[i]->Address, loopstart);
		}
	}
}

//==========================================================================
//
// FxWhileLoop
//
//==========================================================================

FxWhileLoop::FxWhileLoop(FxExpression *condition, FxExpression *code, const FScriptPosition &pos)
: FxLoopStatement(pos), Condition(condition), Code(code)
{
	ValueType = TypeVoid;
}

FxWhileLoop::~FxWhileLoop()
{
	SAFE_DELETE(Condition);
	SAFE_DELETE(Code);
}

FxExpression *FxWhileLoop::DoResolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Condition, ctx);
	SAFE_RESOLVE_OPT(Code, ctx);

	if (Condition->ValueType != TypeBool)
	{
		Condition = new FxBoolCast(Condition);
		SAFE_RESOLVE(Condition, ctx);
	}

	if (Condition->isConstant())
	{
		if (static_cast<FxConstant *>(Condition)->GetValue().GetBool() == false)
		{ // Nothing happens
			FxExpression *nop = new FxNop(ScriptPosition);
			delete this;
			return nop;
		}
		else if (Code == nullptr)
		{ // "while (true) { }"
		  // Someone could be using this for testing.
			ScriptPosition.Message(MSG_WARNING, "Infinite empty loop");
		}
	}

	return this;
}

ExpEmit FxWhileLoop::Emit(VMFunctionBuilder *build)
{
	assert(Condition->ValueType == TypeBool);

	size_t loopstart, loopend;
	size_t jumpspot;

	// Evaluate the condition and execute/break out of the loop.
	loopstart = build->GetAddress();
	if (!Condition->isConstant())
	{
		ExpEmit cond = Condition->Emit(build);
		build->Emit(OP_TEST, cond.RegNum, 0);
		jumpspot = build->Emit(OP_JMP, 0);
		cond.Free(build);
	}
	else assert(static_cast<FxConstant *>(Condition)->GetValue().GetBool() == true);

	// Execute the loop's content.
	if (Code != nullptr)
	{
		ExpEmit code = Code->Emit(build);
		code.Free(build);
	}

	// Loop back.
	build->Backpatch(build->Emit(OP_JMP, 0), loopstart);
	loopend = build->GetAddress();

	if (!Condition->isConstant()) 
	{
		build->Backpatch(jumpspot, loopend);
	}

	Backpatch(build, loopstart, loopend);
	return ExpEmit();
}

//==========================================================================
//
// FxDoWhileLoop
//
//==========================================================================

FxDoWhileLoop::FxDoWhileLoop(FxExpression *condition, FxExpression *code, const FScriptPosition &pos)
: FxLoopStatement(pos), Condition(condition), Code(code)
{
	ValueType = TypeVoid;
}

FxDoWhileLoop::~FxDoWhileLoop()
{
	SAFE_DELETE(Condition);
	SAFE_DELETE(Code);
}

FxExpression *FxDoWhileLoop::DoResolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Condition, ctx);
	SAFE_RESOLVE_OPT(Code, ctx);

	if (Condition->ValueType != TypeBool)
	{
		Condition = new FxBoolCast(Condition);
		SAFE_RESOLVE(Condition, ctx);
	}

	if (Condition->isConstant())
	{
		if (static_cast<FxConstant *>(Condition)->GetValue().GetBool() == false)
		{ // The code executes once, if any.
			if (Jumps.Size() == 0)
			{ // We would still have to handle the jumps however.
				FxExpression *e = Code;
				if (e == nullptr) e = new FxNop(ScriptPosition);
				Code = nullptr;
				delete this;
				return e;
			}
		}
		else if (Code == nullptr)
		{ // "do { } while (true);"
		  // Someone could be using this for testing.
			ScriptPosition.Message(MSG_WARNING, "Infinite empty loop");
		}
	}

	return this;
}

ExpEmit FxDoWhileLoop::Emit(VMFunctionBuilder *build)
{
	assert(Condition->ValueType == TypeBool);

	size_t loopstart, loopend;
	size_t codestart;

	// Execute the loop's content.
	codestart = build->GetAddress();
	if (Code != nullptr)
	{
		ExpEmit code = Code->Emit(build);
		code.Free(build);
	}

	// Evaluate the condition and execute/break out of the loop.
	loopstart = build->GetAddress();
	if (!Condition->isConstant())
	{
		ExpEmit cond = Condition->Emit(build);
		build->Emit(OP_TEST, cond.RegNum, 1);
		cond.Free(build);
		build->Backpatch(build->Emit(OP_JMP, 0), codestart);
	}
	else if (static_cast<FxConstant *>(Condition)->GetValue().GetBool() == true)
	{ // Always looping
		build->Backpatch(build->Emit(OP_JMP, 0), codestart);
	}
	loopend = build->GetAddress();

	Backpatch(build, loopstart, loopend);

	return ExpEmit();
}

//==========================================================================
//
// FxForLoop
//
//==========================================================================

FxForLoop::FxForLoop(FxExpression *init, FxExpression *condition, FxExpression *iteration, FxExpression *code, const FScriptPosition &pos)
: FxLoopStatement(pos), Init(init), Condition(condition), Iteration(iteration), Code(code)
{
	ValueType = TypeVoid;
}

FxForLoop::~FxForLoop()
{
	SAFE_DELETE(Init);
	SAFE_DELETE(Condition);
	SAFE_DELETE(Iteration);
	SAFE_DELETE(Code);
}

FxExpression *FxForLoop::DoResolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE_OPT(Init, ctx);
	SAFE_RESOLVE_OPT(Condition, ctx);
	SAFE_RESOLVE_OPT(Iteration, ctx);
	SAFE_RESOLVE_OPT(Code, ctx);

	if (Condition != nullptr)
	{
		if (Condition->ValueType != TypeBool)
		{
			Condition = new FxBoolCast(Condition);
			SAFE_RESOLVE(Condition, ctx);
		}

		if (Condition->isConstant())
		{
			if (static_cast<FxConstant *>(Condition)->GetValue().GetBool() == false)
			{ // Nothing happens
				FxExpression *nop = new FxNop(ScriptPosition);
				delete this;
				return nop;
			}
			else
			{ // "for (..; true; ..)"
				delete Condition;
				Condition = nullptr;
			}
		}
	}
	if (Condition == nullptr && Code == nullptr)
	{ // "for (..; ; ..) { }"
	  // Someone could be using this for testing.
		ScriptPosition.Message(MSG_WARNING, "Infinite empty loop");
	}

	return this;
}

ExpEmit FxForLoop::Emit(VMFunctionBuilder *build)
{
	assert((Condition && Condition->ValueType == TypeBool && !Condition->isConstant()) || Condition == nullptr);

	size_t loopstart, loopend;
	size_t codestart;
	size_t jumpspot;

	// Init statement (only used by DECORATE. ZScript is pulling it before the loop statement and enclosing the entire loop in a compound statement so that Init can have local variables.)
	if (Init != nullptr)
	{
		ExpEmit init = Init->Emit(build);
		init.Free(build);
	}

	// Evaluate the condition and execute/break out of the loop.
	codestart = build->GetAddress();
	if (Condition != nullptr)
	{
		ExpEmit cond = Condition->Emit(build);
		build->Emit(OP_TEST, cond.RegNum, 0);
		cond.Free(build);
		jumpspot = build->Emit(OP_JMP, 0);
	}

	// Execute the loop's content.
	if (Code != nullptr)
	{
		ExpEmit code = Code->Emit(build);
		code.Free(build);
	}

	// Iteration statement.
	loopstart = build->GetAddress();
	if (Iteration != nullptr)
	{
		ExpEmit iter = Iteration->Emit(build);
		iter.Free(build);
	}
	build->Backpatch(build->Emit(OP_JMP, 0), codestart);

	// End of loop.
	loopend = build->GetAddress();
	if (Condition != nullptr)
	{
		build->Backpatch(jumpspot, loopend);
	}

	Backpatch(build, loopstart, loopend);
	return ExpEmit();
}

//==========================================================================
//
// FxJumpStatement
//
//==========================================================================

FxJumpStatement::FxJumpStatement(int token, const FScriptPosition &pos)
: FxExpression(pos), Token(token)
{
	ValueType = TypeVoid;
}

FxExpression *FxJumpStatement::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();

	if (ctx.Loop != nullptr)
	{
		ctx.Loop->Jumps.Push(this);
		return this;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "'%s' outside of a loop", Token == TK_Break ? "break" : "continue");
		delete this;
		return nullptr;
	}
}

ExpEmit FxJumpStatement::Emit(VMFunctionBuilder *build)
{
	Address = build->Emit(OP_JMP, 0);

	return ExpEmit();
}

//==========================================================================
//
//==========================================================================

FxReturnStatement::FxReturnStatement(FxExpression *value, const FScriptPosition &pos)
: FxExpression(pos), Value(value)
{
	ValueType = TypeVoid;
}

FxReturnStatement::~FxReturnStatement()
{
	SAFE_DELETE(Value);
}

FxExpression *FxReturnStatement::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE_OPT(Value, ctx);

	PPrototype *retproto;
	if (Value == nullptr)
	{
		TArray<PType *> none(0);
		retproto = NewPrototype(none, none);
	}
	else
	{
		retproto = Value->ReturnProto();
	}

	ctx.CheckReturn(retproto, ScriptPosition);

	return this;
}

ExpEmit FxReturnStatement::Emit(VMFunctionBuilder *build)
{
	ExpEmit out(0, REGT_NIL);

	// If we return nothing, use a regular RET opcode.
	// Otherwise just return the value we're given.
	if (Value == nullptr)
	{
		build->Emit(OP_RET, RET_FINAL, REGT_NIL, 0);
	}
	else
	{
		out = Value->Emit(build);

		// Check if it is a function call that simplified itself
		// into a tail call in which case we don't emit anything.
		if (!out.Final)
		{
			if (Value->ValueType == TypeVoid)
			{ // Nothing is returned.
				build->Emit(OP_RET, RET_FINAL, REGT_NIL, 0);
			}
			else
			{
				build->Emit(OP_RET, RET_FINAL, out.RegType | (out.Konst ? REGT_KONST : 0), out.RegNum);
			}
		}
	}

	out.Final = true;
	return out;
}

VMFunction *FxReturnStatement::GetDirectFunction()
{
	if (Value != nullptr)
	{
		return Value->GetDirectFunction();
	}
	return nullptr;
}

//==========================================================================
//
//==========================================================================

FxClassTypeCast::FxClassTypeCast(PClassPointer *dtype, FxExpression *x)
: FxExpression(x->ScriptPosition)
{
	ValueType = dtype;
	desttype = dtype->ClassRestriction;
	basex=x;
}

//==========================================================================
//
//
//
//==========================================================================

FxClassTypeCast::~FxClassTypeCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxClassTypeCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeNullPtr)
	{
		basex->ValueType = ValueType;
		auto x = basex;
		basex = nullptr;
		delete this;
		return x;
	}
	if (basex->ValueType->GetClass() == RUNTIME_CLASS(PClassPointer))
	{
		auto to = static_cast<PClassPointer *>(ValueType);
		auto from = static_cast<PClassPointer *>(basex->ValueType);
		if (from->ClassRestriction->IsDescendantOf(to->ClassRestriction))
		{
			basex->ValueType = to;
			auto x = basex;
			basex = nullptr;
			delete this;
			return x;
		}
		ScriptPosition.Message(MSG_ERROR, "Cannot convert from %s to %s: Incompatible class types", from->ClassRestriction->TypeName.GetChars(), to->ClassRestriction->TypeName.GetChars());
		delete this;
		return nullptr;
	}
	
	if (basex->ValueType != TypeName && basex->ValueType != TypeString)
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot convert %s to class type", basex->ValueType->DescriptiveName());
		delete this;
		return NULL;
	}

	if (basex->isConstant())
	{
		FName clsname = static_cast<FxConstant *>(basex)->GetValue().GetName();
		PClass *cls = NULL;

		if (clsname != NAME_None)
		{
			cls = PClass::FindClass(clsname);
			if (cls == NULL)
			{
				/* lax */
				// Since this happens in released WADs it must pass without a terminal error... :(
				ScriptPosition.Message(MSG_OPTERROR,
					"Unknown class name '%s'",
					clsname.GetChars(), desttype->TypeName.GetChars());
			}
			else
			{
				if (!cls->IsDescendantOf(desttype))
				{
					ScriptPosition.Message(MSG_ERROR, "class '%s' is not compatible with '%s'", clsname.GetChars(), desttype->TypeName.GetChars());
					delete this;
					return NULL;
				}
				ScriptPosition.Message(MSG_DEBUG, "resolving '%s' as class name", clsname.GetChars());
			}
		}
		FxExpression *x = new FxConstant(cls, ScriptPosition);
		delete this;
		return x;
	}
	if (basex->ValueType == TypeString)
	{
		basex = new FxNameCast(basex);
	}
	return this;
}

//==========================================================================
//
// 
//
//==========================================================================

int DecoNameToClass(VMFrameStack *stack, VMValue *param, int numparam, VMReturn *ret, int numret)
{
	assert(numparam == 2);
	assert(numret == 1);
	assert(param[0].Type == REGT_INT);
	assert(param[1].Type == REGT_POINTER);
	assert(ret->RegType == REGT_POINTER);

	FName clsname = ENamedName(param[0].i);
	const PClass *cls = PClass::FindClass(clsname);
	const PClass *desttype = reinterpret_cast<PClass *>(param[0].a);

	if (!cls->IsDescendantOf(desttype))
	{
		Printf("class '%s' is not compatible with '%s'", clsname.GetChars(), desttype->TypeName.GetChars());
		cls = NULL;
	}
	ret->SetPointer(const_cast<PClass *>(cls), ATAG_OBJECT);
	return 1;
}

ExpEmit FxClassTypeCast::Emit(VMFunctionBuilder *build)
{
	if (basex->ValueType != TypeName)
	{
		return ExpEmit(build->GetConstantAddress(NULL, ATAG_OBJECT), REGT_POINTER, true);
	}
	ExpEmit clsname = basex->Emit(build);
	assert(!clsname.Konst);
	ExpEmit dest(build, REGT_POINTER);
	build->Emit(OP_PARAM, 0, clsname.RegType, clsname.RegNum);
	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(const_cast<PClass *>(desttype), ATAG_OBJECT));

	// Call the DecoNameToClass function to convert from 'name' to class.
	VMFunction *callfunc;
	PSymbol *sym = FindDecorateBuiltinFunction(NAME_DecoNameToClass, DecoNameToClass);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != NULL);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	build->Emit(OP_CALL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 2, 1);
	build->Emit(OP_RESULT, 0, REGT_POINTER, dest.RegNum);
	clsname.Free(build);
	return dest;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxStateByIndex::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	ABORT(ctx.Class);
	auto aclass = dyn_cast<PClassActor>(ctx.Class);

	// This expression type can only be used from DECORATE, so there's no need to consider the possibility of calling it from a non-actor.
	assert(aclass != nullptr && aclass->NumOwnedStates > 0);

	if (aclass->NumOwnedStates <= index)
	{
		ScriptPosition.Message(MSG_ERROR, "%s: Attempt to jump to non existing state index %d", 
			ctx.Class->TypeName.GetChars(), index);
		delete this;
		return NULL;
	}
	FxExpression *x = new FxConstant(aclass->OwnedStates + index, ScriptPosition);
	delete this;
	return x;
}

//==========================================================================
//
//
//
//==========================================================================

FxRuntimeStateIndex::FxRuntimeStateIndex(FxExpression *index)
: FxExpression(index->ScriptPosition), Index(index)
{
	EmitTail = false;
	ValueType = TypeState;
}

FxRuntimeStateIndex::~FxRuntimeStateIndex()
{
	SAFE_DELETE(Index);
}

PPrototype *FxRuntimeStateIndex::ReturnProto()
{
	EmitTail = true;
	return FxExpression::ReturnProto();
}

FxExpression *FxRuntimeStateIndex::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Index, ctx);

	if (!Index->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return nullptr;
	}
	else if (Index->ValueType->GetRegType() != REGT_INT)
	{ // Float.
		Index = new FxIntCast(Index, ctx.FromDecorate);
		SAFE_RESOLVE(Index, ctx);
	}

	return this;
}

static bool VerifyJumpTarget(AActor *stateowner, FStateParamInfo *stateinfo, int index)
{
	PClassActor *cls = stateowner->GetClass();

	if (stateinfo->mCallingState != nullptr)
	{
	while (cls != RUNTIME_CLASS(AActor))
	{
		// both calling and target state need to belong to the same class.
		if (cls->OwnsState(stateinfo->mCallingState))
		{
			return cls->OwnsState(stateinfo->mCallingState + index);
		}

		// We can safely assume the ParentClass is of type PClassActor
		// since we stop when we see the Actor base class.
		cls = static_cast<PClassActor *>(cls->ParentClass);
	}
	}
	return false;
}

static int DecoHandleRuntimeState(VMFrameStack *stack, VMValue *param, int numparam, VMReturn *ret, int numret)
{
	PARAM_PROLOGUE;
	PARAM_OBJECT(stateowner, AActor);
	PARAM_POINTER(stateinfo, FStateParamInfo);
	PARAM_INT(index);

	if (index == 0 || !VerifyJumpTarget(stateowner, stateinfo, index))
	{
		// Null is returned if the location was invalid which means that no jump will be performed
		// if used as return value
		// 0 always meant the same thing so we handle it here for compatibility
		ACTION_RETURN_STATE(nullptr);
	}
	else
	{
		ACTION_RETURN_STATE(stateinfo->mCallingState + index);
	}
}

ExpEmit FxRuntimeStateIndex::Emit(VMFunctionBuilder *build)
{
	assert(build->IsActionFunc && build->Registers[REGT_POINTER].GetMostUsed() >= 3 &&
		"FxRuntimeStateIndex is only valid inside action functions");

	ExpEmit out(build, REGT_POINTER);

	build->Emit(OP_PARAM, 0, REGT_POINTER, 1); // stateowner
	build->Emit(OP_PARAM, 0, REGT_POINTER, 2); // stateinfo
	ExpEmit id = Index->Emit(build);
	build->Emit(OP_PARAM, 0, REGT_INT | (id.Konst ? REGT_KONST : 0), id.RegNum); // index

	VMFunction *callfunc;
	PSymbol *sym;
	
	sym = FindDecorateBuiltinFunction(NAME_DecoHandleRuntimeState, DecoHandleRuntimeState);
	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != nullptr);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	if (EmitTail)
	{
		build->Emit(OP_TAIL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 3, 1);
		out.Final = true;
	}
	else
	{
		build->Emit(OP_CALL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 3, 1);
		build->Emit(OP_RESULT, 0, REGT_POINTER, out.RegNum);
	}

	return out;
}

//==========================================================================
//
//
//
//==========================================================================

FxMultiNameState::FxMultiNameState(const char *_statestring, const FScriptPosition &pos)
	:FxExpression(pos)
{
	FName scopename;
	FString statestring = _statestring;
	int scopeindex = statestring.IndexOf("::");

	if (scopeindex >= 0)
	{
		scopename = FName(statestring, scopeindex, false);
		statestring = statestring.Right(statestring.Len() - scopeindex - 2);
	}
	else
	{
		scopename = NAME_None;
	}
	names = MakeStateNameList(statestring);
	names.Insert(0, scopename);
	scope = NULL;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxMultiNameState::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	ABORT(ctx.Class);

	if (names[0] == NAME_None)
	{
		scope = NULL;
	}
	else if (names[0] == NAME_Super)
	{
		scope = dyn_cast<PClassActor>(ctx.Class->ParentClass);
	}
	else
	{
		scope = PClass::FindActor(names[0]);
		if (scope == NULL)
		{
			ScriptPosition.Message(MSG_ERROR, "Unknown class '%s' in state label", names[0].GetChars());
			delete this;
			return NULL;
		}
		else if (!scope->IsAncestorOf(ctx.Class))
		{
			ScriptPosition.Message(MSG_ERROR, "'%s' is not an ancestor of '%s'", names[0].GetChars(), ctx.Class->TypeName.GetChars());
			delete this;
			return NULL;
		}
	}
	if (scope != NULL)
	{
		FState *destination = NULL;
		// If the label is class specific we can resolve it right here
		if (names[1] != NAME_None)
		{
			destination = scope->FindState(names.Size()-1, &names[1], false);
			if (destination == NULL)
			{
				ScriptPosition.Message(MSG_OPTERROR, "Unknown state jump destination");
				/* lax */
				return this;
			}
		}
		FxExpression *x = new FxConstant(destination, ScriptPosition);
		delete this;
		return x;
	}
	names.Delete(0);
	names.ShrinkToFit();
	ValueType = TypeState;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

static int DoFindState(VMFrameStack *stack, VMValue *param, int numparam, VMReturn *ret, FName *names, int numnames)
{
	PARAM_OBJECT_AT(0, self, AActor);
	FState *state = self->GetClass()->FindState(numparam - 1, names);
	if (state == NULL)
	{
		const char *dot = "";
		Printf("Jump target '");
 		for (int i = 0; i < numparam - 1; i++)
		{
			Printf("%s%s", dot, names[i].GetChars());
			dot = ".";
		}
		Printf("' not found in %s\n", self->GetClass()->TypeName.GetChars());
	}
	ret->SetPointer(state, ATAG_STATE);
	return 1;
}

// Find a state with any number of dots in its name.
int BuiltinFindMultiNameState(VMFrameStack *stack, VMValue *param, int numparam, VMReturn *ret, int numret)
{
	assert(numparam > 1);
	assert(numret == 1);
	assert(ret->RegType == REGT_POINTER);

	FName *names = (FName *)alloca((numparam - 1) * sizeof(FName));
	for (int i = 1; i < numparam; ++i)
	{
		PARAM_NAME_AT(i, zaname);
		names[i - 1] = zaname;
	}
	return DoFindState(stack, param, numparam, ret, names, numparam - 1);
}

// Find a state without any dots in its name.
int BuiltinFindSingleNameState(VMFrameStack *stack, VMValue *param, int numparam, VMReturn *ret, int numret)
{
	assert(numparam == 2);
	assert(numret == 1);
	assert(ret->RegType == REGT_POINTER);

	PARAM_NAME_AT(1, zaname);
	return DoFindState(stack, param, numparam, ret, &zaname, 1);
}

ExpEmit FxMultiNameState::Emit(VMFunctionBuilder *build)
{
	ExpEmit dest(build, REGT_POINTER);
	if (build->IsActionFunc)
	{
		build->Emit(OP_PARAM, 0, REGT_POINTER, 1);		// pass stateowner
	}
	else
	{
		build->Emit(OP_PARAM, 0, REGT_POINTER, 0);		// pass self
	}
	for (unsigned i = 0; i < names.Size(); ++i)
	{
		build->EmitParamInt(names[i]);
	}

	// For one name, use the BuiltinFindSingleNameState function. For more than
	// one name, use the BuiltinFindMultiNameState function.
	VMFunction *callfunc;
	PSymbol *sym;
	
	if (names.Size() == 1)
	{
		sym = FindDecorateBuiltinFunction(NAME_BuiltinFindSingleNameState, BuiltinFindSingleNameState);
	}
	else
	{
		sym = FindDecorateBuiltinFunction(NAME_BuiltinFindMultiNameState, BuiltinFindMultiNameState);
	}

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != NULL);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	build->Emit(OP_CALL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), names.Size() + 1, 1);
	build->Emit(OP_RESULT, 0, REGT_POINTER, dest.RegNum);
	return dest;
}

//==========================================================================
//
//
//
//==========================================================================

FxDamageValue::FxDamageValue(FxExpression *v)
: FxExpression(v->ScriptPosition)
{
	val = v;
	ValueType = TypeVoid;
}

FxDamageValue::~FxDamageValue()
{
	SAFE_DELETE(val);

}

FxExpression *FxDamageValue::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(val, ctx)

	if (!val->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return NULL;
	}
	return this;
}

// This is a highly-specialized "expression" type that emits a complete function.
ExpEmit FxDamageValue::Emit(VMFunctionBuilder *build)
{
	if (val->isConstant())
	{
		build->EmitRetInt(0, false, static_cast<FxConstant *>(val)->GetValue().Int);
	}
	else
	{
		ExpEmit emitval = val->Emit(build);
		assert(emitval.RegType == REGT_INT);
		build->Emit(OP_RET, 0, REGT_INT | (emitval.Konst ? REGT_KONST : 0), emitval.RegNum);
	}
	build->Emit(OP_RETI, 1 | RET_FINAL, true);

	return ExpEmit();
}

//==========================================================================
//
// declares a single local variable (no arrays)
//
//==========================================================================

FxLocalVariableDeclaration::FxLocalVariableDeclaration(PType *type, FName name, FxExpression *initval, int varflags, const FScriptPosition &p)
	:FxExpression(p)
{
	ValueType = type;
	VarFlags = varflags;
	Name = name;
	Init = initval == nullptr? nullptr : new FxTypeCast(initval, type, false);
}

FxExpression *FxLocalVariableDeclaration::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE_OPT(Init, ctx);
	if (ctx.Block == nullptr)
	{
		ScriptPosition.Message(MSG_ERROR, "Variable declaration outside compound statement");
		delete this;
		return nullptr;
	}
	ctx.Block->LocalVars.Push(this);
	return this;
}

ExpEmit FxLocalVariableDeclaration::Emit(VMFunctionBuilder *build)
{
	if (Init == nullptr)
	{
		RegNum = build->Registers[ValueType->GetRegType()].Get(1);
	}
	else
	{
		ExpEmit emitval = Init->Emit(build);

		int regtype = emitval.RegType;
		if (regtype < REGT_INT || regtype > REGT_TYPE)
		{
			ScriptPosition.Message(MSG_ERROR, "Attempted to assign a non-value");
			return ExpEmit();
		}
		if (emitval.Konst)
		{
			auto constval = static_cast<FxConstant *>(Init);
			RegNum = build->Registers[regtype].Get(1);
			switch (regtype)
			{
			default:
			case REGT_INT:
				build->Emit(OP_LK, build->GetConstantInt(constval->GetValue().GetInt()), RegNum);
				break;

			case REGT_FLOAT:
				build->Emit(OP_LKF, build->GetConstantFloat(constval->GetValue().GetFloat()), RegNum);
				break;

			case REGT_POINTER:
				build->Emit(OP_LKP, build->GetConstantAddress(constval->GetValue().GetPointer(), ATAG_GENERIC), RegNum);
				break;

			case REGT_STRING:
				build->Emit(OP_LKS, build->GetConstantString(constval->GetValue().GetString()), RegNum);
			}
			emitval.Free(build);
		}
		else
		{
			// take over the register that got allocated while emitting the Init expression.
			RegNum = emitval.RegNum;
		}
	}
	return ExpEmit();
}

void FxLocalVariableDeclaration::Release(VMFunctionBuilder *build)
{
	// Release the register after the containing block gets closed
	assert(RegNum != -1);
	build->Registers[ValueType->GetRegType()].Return(RegNum, 1);
}
