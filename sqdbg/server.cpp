//-----------------------------------------------------------------------
//                       github.com/samisalreadytaken/sqdbg
//-----------------------------------------------------------------------
//
// Squirrel Debugger
//

#define SQDBG_SV_VER 1

#include "sqdbg.h"
#include "net.h"

#ifndef _WIN32
#include <ctype.h>
#endif
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <float.h>
#include <new>

#include <squirrel.h>
#include <sqstdaux.h>
#include <sqobject.h>
#include <sqstate.h>
#include <sqcompiler.h>
#include <sqvm.h>
#include <sqarray.h>
#include <sqtable.h>
#include <sqfuncproto.h>
#include <sqfuncstate.h>
#include <sqlexer.h>
#include <sqclosure.h>
#include <sqclass.h>
#include <sqstring.h>
#include <squserdata.h>

#include "protocol.h"
#include "str.h"
#include "json.h"

#ifdef _WIN32
	void Sleep( int ms )
	{
		::Sleep( (DWORD)ms );
	}
#else
	#include <time.h>
	void Sleep( int ms )
	{
		timespec t;
		t.tv_nsec = ms * 1000000;
		t.tv_sec = 0;
		nanosleep( &t, NULL );
	}
#endif

#ifndef _WIN32
#undef offsetof
#define offsetof(a,b) ((size_t)(&(((a*)0)->b)))
#endif

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

inline int clamp( int val, int min, int max )
{
	if ( val < min )
		return min;

	if ( val > max )
		return max;

	return val;
}

#undef _SC
#ifdef SQUNICODE
	#define _SC(s) __CAT( L, s )
#else
	#define _SC(s) s
#endif

#ifndef scstrchr
#ifdef SQUNICODE
	#define scstrchr wcsrchr
#else
	#define scstrchr strchr
#endif
#endif

#ifndef scstricmp
#ifdef SQUNICODE
	#ifdef _WIN32
		#define scstricmp _wcsicmp
	#else
		#define scstricmp wcscmp
	#endif
#else
	#ifdef _WIN32
		#define scstricmp _stricmp
	#else
		#define scstricmp strcasecmp
	#endif
#endif
#endif

#ifndef SQUIRREL_VERSION_NUMBER
	#define SQUIRREL_VERSION_NUMBER 223
#endif

#if SQUIRREL_VERSION_NUMBER >= 300
	#define _fp(func) (func)
	#define _outervalptr(outervar) (_outer((outervar))->_valptr)
	#define _nativenoutervalues(p) (p)->_noutervalues
	#define SUPPORTS_RESTART_FRAME
	#define NATIVE_DEBUG_HOOK

	#if SQUIRREL_VERSION_NUMBER >= 310
		#define CLOSURE_ROOT
	#endif
#else
	#define _fp(func) _funcproto(func)
	#define _outervalptr(outervar) (&(outervar))
	#define sq_rsl(l) ((l)*(sizeof(SQChar)/sizeof(char)))
	#define _nativenoutervalues(p) (p)->_outervalues.size()

	#ifndef SQUNICODE
		#undef scvsprintf
		#define scvsprintf vsnprintf
	#endif
	#undef _rawval
	#define _rawval(o) (uintptr_t)((o)._unVal.pRefCounted)

	#undef scsprintf
	#ifdef SQUNICODE
		#define scsprintf swprintf
	#else
		#define scsprintf snprintf
	#endif
#endif

#if SQUIRREL_VERSION_NUMBER >= 212
	#define CLOSURE_ENV

	#if SQUIRREL_VERSION_NUMBER >= 300
		#define CLOSURE_ENV_ISVALID( env ) (env)
		#define CLOSURE_ENV_OBJ( env ) ((env)->_obj)
	#else
		#define CLOSURE_ENV_ISVALID( env ) (sq_type(env) == OT_WEAKREF && _weakref(env))
		#define CLOSURE_ENV_OBJ( env ) (_weakref(env)->_obj)
	#endif
#endif

#define GetSQClassLocked( pClass ) (pClass)->_locked
#define SetSQClassLocked( pClass, v ) (pClass)->_locked = (v)

#define SQ_FOREACH_OP( obj, key, val ) \
		{ \
			int _jump; \
			for ( SQObjectPtr _pos, key, val; \
					m_pCurVM->FOREACH_OP( obj, key, val, _pos, 0, 666, _jump ) && \
					_jump != 666; ) \
			{

#define SQ_FOREACH_END() } }

#define FOREACH_SQTABLE( pTable, key, val )\
	SQInteger _i = 0;\
	for ( SQObjectPtr pi = _i; (_i = pTable->Next( false, pi, key, val )) != -1; pi._unVal.nInteger = _i )

#define FOREACH_SQCLASS( pClass, key, val )\
	SQInteger _i = 0;\
	for ( SQObjectPtr pi = _i; (_i = pClass->Next( pi, key, val )) != -1; pi._unVal.nInteger = _i )

#ifndef SQDBG_EXCLUDE_DEFAULT_MEMFUNCTIONS
inline void *sqdbg_malloc( unsigned int size )
{
	extern void *sq_vm_malloc( SQUnsignedInteger size );
	return sq_vm_malloc( size );
}

inline void *sqdbg_realloc( void *p, unsigned int oldsize, unsigned int size )
{
	extern void *sq_vm_realloc( void *p, SQUnsignedInteger oldsize, SQUnsignedInteger size );
	return sq_vm_realloc( p, oldsize, size );
}

inline void sqdbg_free( void *p, unsigned int size )
{
	extern void sq_vm_free( void *p, SQUnsignedInteger size );
	sq_vm_free( p, size );
}
#endif

template < typename T >
class CRestoreVar
{
public:
	T *var;
	T val;
	CRestoreVar( T *p, T v ) : var(p), val(v) {}
	~CRestoreVar() { if ( var ) *var = val; }
};

#ifdef _DEBUG
class CStackCheck
{
private:
	int top;
	HSQUIRRELVM vm;

public:
	CStackCheck( HSQUIRRELVM vm )
	{
		this->vm = vm;
		top = vm->_top;
	}

	~CStackCheck()
	{
		Assert( vm->_top == top );
	}
};
#else
class CStackCheck
{
public:
	CStackCheck( HSQUIRRELVM ) {}
};
#endif

//
// Longest return value is 16 bytes including nul byte
//
inline string_t GetType( const SQObjectPtr &obj )
{
	switch ( _RAW_TYPE( sq_type(obj) ) )
	{
		case _RT_NULL: return "null";
		case _RT_INTEGER: return "integer";
		case _RT_FLOAT: return "float";
		case _RT_BOOL: return "bool";
		case _RT_STRING: return "string";
		case _RT_TABLE: return "table";
		case _RT_ARRAY: return "array";
		case _RT_GENERATOR: return "generator";
		case _RT_CLOSURE: return "function";
		case _RT_NATIVECLOSURE: return "native function";
		case _RT_USERDATA:
		case _RT_USERPOINTER: return "userdata";
		case _RT_THREAD: return "thread";
		case _RT_FUNCPROTO: return "function";
		case _RT_CLASS: return "class";
		case _RT_INSTANCE: return "instance";
		case _RT_WEAKREF: return "weakref";
#if SQUIRREL_VERSION_NUMBER >= 300
		case _RT_OUTER: return "outer";
#endif
		default: Assert(!"unknown type"); return "unknown";
	}
}

#if SQUIRREL_VERSION_NUMBER >= 300
string_t const g_InstructionName[ _OP_CLOSE + 1 ]=
{
	"LINE",
	"LOAD",
	"LOADINT",
	"LOADFLOAT",
	"DLOAD",
	"TAILCALL",
	"CALL",
	"PREPCALL",
	"PREPCALLK",
	"GETK",
	"MOVE",
	"NEWSLOT",
	"DELETE",
	"SET",
	"GET",
	"EQ",
	"NE",
	"ADD",
	"SUB",
	"MUL",
	"DIV",
	"MOD",
	"BITW",
	"RETURN",
	"LOADNULLS",
	"LOADROOT",
	"LOADBOOL",
	"DMOVE",
	"JMP",
	"JCMP",
	"JZ",
	"SETOUTER",
	"GETOUTER",
	"NEWOBJ",
	"APPENDARRAY",
	"COMPARITH",
	"INC",
	"INCL",
	"PINC",
	"PINCL",
	"CMP",
	"EXISTS",
	"INSTANCEOF",
	"AND",
	"OR",
	"NEG",
	"NOT",
	"BWNOT",
	"CLOSURE",
	"YIELD",
	"RESUME",
	"FOREACH",
	"POSTFOREACH",
	"CLONE",
	"TYPEOF",
	"PUSHTRAP",
	"POPTRAP",
	"THROW",
	"NEWSLOTA",
	"GETBASE",
	"CLOSE",
};
#elif SQUIRREL_VERSION_NUMBER >= 212
string_t const g_InstructionName[ _OP_NEWSLOTA + 1 ]=
{
	"LINE",
	"LOAD",
	"LOADINT",
	"LOADFLOAT",
	"DLOAD",
	"TAILCALL",
	"CALL",
	"PREPCALL",
	"PREPCALLK",
	"GETK",
	"MOVE",
	"NEWSLOT",
	"DELETE",
	"SET",
	"GET",
	"EQ",
	"NE",
	"ARITH",
	"BITW",
	"RETURN",
	"LOADNULLS",
	"LOADROOTTABLE",
	"LOADBOOL",
	"DMOVE",
	"JMP",
	"JNZ",
	"JZ",
	"LOADFREEVAR",
	"VARGC",
	"GETVARGV",
	"NEWTABLE",
	"NEWARRAY",
	"APPENDARRAY",
	"GETPARENT",
	"COMPARITH",
	"COMPARITHL",
	"INC",
	"INCL",
	"PINC",
	"PINCL",
	"CMP",
	"EXISTS",
	"INSTANCEOF",
	"AND",
	"OR",
	"NEG",
	"NOT",
	"BWNOT",
	"CLOSURE",
	"YIELD",
	"RESUME",
	"FOREACH",
	"POSTFOREACH",
	"DELEGATE",
	"CLONE",
	"TYPEOF",
	"PUSHTRAP",
	"POPTRAP",
	"THROW",
	"CLASS",
	"NEWSLOTA",
};
#endif

#define MT_LAST SQMetaMethod::MT_LAST

string_t const g_MetaMethodName[ MT_LAST ] =
{
	"_add",
	"_sub",
	"_mul",
	"_div",
	"_unm",
	"_modulo",
	"_set",
	"_get",
	"_typeof",
	"_nexti",
	"_cmp",
	"_call",
	"_cloned",
	"_newslot",
	"_delslot",
#if SQUIRREL_VERSION_NUMBER >= 210
	"_tostring",
	"_newmember",
	"_inherited",
#endif
};

inline SQObject ToSQObject( SQClass *val )
{
	SQObject obj;
	obj._type = OT_CLASS;
	obj._unVal.pClass = val;
	return obj;
}

inline SQObject ToSQObject( SQTable *val )
{
	SQObject obj;
	obj._type = OT_TABLE;
	obj._unVal.pTable = val;
	return obj;
}

inline SQString *CreateSQString( SQSharedState *ss, const string_t &str )
{
#ifdef SQUNICODE
	Assert( str.IsTerminated() );
	Assert( UnicodeLength( str.ptr ) < 1024 );

	SQChar tmp[1024];
	return SQString::Create( ss, tmp, UTF8ToUnicode( tmp, str.ptr, sizeof(tmp) ) );
#else
	return SQString::Create( ss, str.ptr, str.len );
#endif
}

inline SQString *CreateSQString( HSQUIRRELVM vm, const string_t &str )
{
	return CreateSQString( vm->_sharedstate, str );
}

inline bool SQTable_Get( SQTable *table, const string_t &key, SQObjectPtr &val )
{
#if SQUIRREL_VERSION_NUMBER >= 300
	#ifdef SQUNICODE
		Assert( key.IsTerminated() );
		Assert( UnicodeLength( key.ptr ) < 256 );

		SQChar tmp[256];
		return table->GetStr( tmp, UTF8ToUnicode( tmp, key.ptr, sizeof(tmp) ), val );
	#else
		return table->GetStr( key.ptr, key.len, val );
	#endif
#else
	SQObjectPtr str = CreateSQString( table->_sharedstate, key );
	return table->Get( str, val );
#endif
}

#ifdef SQUNICODE
inline bool SQTable_Get( SQTable *table, const sqstring_t &key, SQObjectPtr &val )
{
#if SQUIRREL_VERSION_NUMBER >= 300
	return table->GetStr( key.ptr, key.len, val );
#else
	SQObjectPtr str = SQString::Create( table->_sharedstate, key.ptr, key.len );
	return table->Get( str, val );
#endif
}
#endif


#define SQDBG_SV_TAG "__sqdbg__"

#define KW_DELEGATE "@delegate"
#define KW_THIS "__this"

#define INTERNAL_TAG( name ) "$" name

typedef enum
{
	NONE		= 0x0000,
	HEX			= 0x0001,
	BIN			= 0x0002,
	DEC			= 0x0004,
	FLT			= 0x0008,
	NO_QUOT		= 0x0010,
	CLAMP		= 0x0020,
	PURE		= 0x0040,
	LOCK		= 0x1000,
} VARSPEC;

struct BreakReason
{
	typedef enum
	{
		None = 0,
		Step = 1,
		Breakpoint,
		Exception,
		Pause,
		Restart,
		Goto,
		FunctionBreakpoint,
		DataBreakpoint,
	} EBreakReason;

	EBreakReason reason;
	string_t text;
	int id;

	BreakReason() { memset( this, 0, sizeof(*this) ); }
};

struct breakpoint_t
{
	int line;
	sqstring_t src;
	sqstring_t funcsrc;

	SQObjectPtr conditionFn;
	SQObjectPtr conditionEnv;

	int hitsTarget;
	int hits;
	string_t logMessage;

	int id;
};

inline HSQUIRRELVM GetThread( SQWeakRef *wr )
{
	Assert( sq_type( wr->_obj ) == OT_THREAD );
	return _thread( wr->_obj );
}

typedef enum
{
	VARREF_OBJ = 0,
	VARREF_SCOPE_LOCALS,
	VARREF_SCOPE_OUTERS,
	VARREF_INSTRUCTIONS,
	VARREF_OUTERS,
	VARREF_LITERALS,
	VARREF_METAMETHODS,
	VARREF_CALLSTACK,
	VARREF_MAX
} EVARREF;

inline bool IsScopeRef( EVARREF type )
{
	Assert( type >= 0 && type < VARREF_MAX );
	return ( type == VARREF_SCOPE_LOCALS || type == VARREF_SCOPE_OUTERS );
}

inline bool IsObjectRef( EVARREF type )
{
	return !IsScopeRef( type );
}

struct varref_t
{
	EVARREF type;
	union
	{
		struct
		{
			int stack;
			SQWeakRef *thread;
		} scope;

		struct
		{
			union
			{
				SQWeakRef *weakref;
				SQObject obj;
			};

			int isWeakref;
			int hasNonStringMembers;
		} obj;
	};

	int id;

	HSQUIRRELVM GetThread() const
	{
		Assert( IsScopeRef( type ) );
		Assert( scope.thread );
		Assert( sq_type( scope.thread->_obj ) == OT_THREAD );
		return _thread( scope.thread->_obj );
	}

	const SQObject &GetVar() const
	{
		Assert( IsObjectRef( type ) );
		Assert( !obj.isWeakref || obj.weakref );
		return obj.isWeakref ? obj.weakref->_obj : obj.obj;
	}
};

struct watch_t
{
	string_t expression;
	SQWeakRef *thread;
	int frame;
};

struct classdef_t
{
	SQClass *base;
	string_t name;
	SQObjectPtr value;
	SQObjectPtr metamembers;
};

struct frameid_t
{
	int frame;
	int threadId;
};

struct script_t
{
	string_t source;
	char *scriptptr;
	int scriptlen;
	int scriptbufsize;
};

struct objref_t
{
	typedef enum
	{
		INVALID = 0,
		PTR = 1,
		TABLE,
		INSTANCE,
		CLASS,
		INSTANCE_META,
		TABLE_META,
		USERDATA_META,
		METAMETHOD_CLASS,
		ARRAY,
	} EOBJPTR;

	EOBJPTR type;

	union
	{
		SQObjectPtr *ptr;
	};

	// Always save key for data watches
	struct
	{
		SQObjectValue src;
		SQObject key;
	};
};

struct datawatch_t
{
	SQWeakRef *container;
	objref_t obj;
	string_t name;

	SQObject oldvalue;

	int hitsTarget;
	int hits;

	int hasCondition;
	SQObject condition;

	int id;
};

typedef enum
{
	Running,
	Suspended,
	NextLine,
	NextStatement,
	StepIn,
	StepOut,
	SuspendNow,
} ThreadState;

//
// Squirrel doesn't read files, it usually keeps file names passed in from host programs.
// DAP returns file path on breakpoints; try to construct file paths from these partial
// file names. This will not work for multiple files with identical names and for files
// where breakpoints were not set.
//
class CFilePathMap
{
public:
	struct pair_t
	{
		sqstring_t name;
		sqstring_t path;
	};

	sqvector< pair_t > map;

	~CFilePathMap()
	{
		Clear();
	}

	void Add( const string_t &name, const string_t &path )
	{
#ifdef SQUNICODE
		Assert( name.IsTerminated() );
		Assert( path.IsTerminated() );

		Assert( UnicodeLength( name.ptr ) < 256 );
		Assert( UnicodeLength( path.ptr ) < 256 );

		SQChar pName[256], pPath[256];
		sqstring_t strName( pName, UTF8ToUnicode( pName, name.ptr, sizeof(pName) ) );
		sqstring_t strPath( pPath, UTF8ToUnicode( pPath, path.ptr, sizeof(pPath) ) );
#else
		const sqstring_t &strName = name;
		const sqstring_t &strPath = path;
#endif

		for ( unsigned int i = 0; i < map.size(); i++ )
		{
			pair_t &v = map[i];
			if ( v.name.IsEqualTo( strName ) )
			{
				if ( !v.path.IsEqualTo( strPath ) )
				{
					v.path.FreeAndCopy( strPath );
				}

				return;
			}
		}

		pair_t &v = map.push_back();
		v.name.Copy( strName );
		v.path.Copy( strPath );
	}

	sqstring_t *Get( const sqstring_t &name )
	{
		for ( unsigned int i = 0; i < map.size(); i++ )
		{
			pair_t &v = map[i];
			if ( v.name.IsEqualTo( name ) )
				return &v.path;
		}

		return NULL;
	}

	void Clear()
	{
		for ( unsigned int i = 0; i < map.size(); i++ )
		{
			pair_t &v = map[i];
			v.name.Free();
			v.path.Free();
		}

		map.resize(0);
	}
};

#define Print(...) \
	m_Print( m_pCurVM, __VA_ARGS__ ); \

#define PrintError(...) \
	m_PrintError( m_pCurVM, __VA_ARGS__ ); \

struct SQDebugServer
{
private:
	ThreadState m_State;
	int m_nStateCalls;
	int m_nCalls;

	HSQUIRRELVM m_pRootVM;
	HSQUIRRELVM m_pCurVM;
	SQObjectPtr m_ReturnValue;

	SQPRINTFUNCTION m_Print;
	SQPRINTFUNCTION m_PrintError;
	SQObjectPtr m_ErrorHandler;

	bool m_bBreakOnExceptions;
	bool m_bIgnoreDebugHookGuard;
	bool m_bDebugHookGuard;
#if SQUIRREL_VERSION_NUMBER < 300
	bool m_bInDebugHook;
	bool m_bExceptionPause;
#endif

	// sq_notifyallexceptions will notify ignored sq_call errors, keep manual guard
	bool m_bIgnoreExceptions;
	HSQUIRRELVM m_pPausedThread;

	// Ignore debug hook calls from the debugger executed scripts
	class CCallGuard
	{
		SQDebugServer *dbg;
		SQObjectPtr temp_reg;

	public:
		CCallGuard( SQDebugServer *p ) :
			dbg( p ),
			temp_reg( p->m_pCurVM->temp_reg )
		{
			dbg->m_bDebugHookGuard = true;
			dbg->m_bIgnoreExceptions = true;
		}

		~CCallGuard()
		{
			dbg->m_bDebugHookGuard = false;
			dbg->m_bIgnoreExceptions = false;
			dbg->m_pCurVM->temp_reg = temp_reg;
		}
	};

private:
	SQObjectPtr m_EnvGetVal;
	SQObjectPtr m_EnvGetKey;

	SQObjectPtr m_sqstrDelegate;
	SQObjectPtr m_sqstrThis;

	BreakReason m_BreakReason;

	int m_Sequence;

	// Indices are valid for the duration of the debug session
	int m_nBreakpointIndex;
	int m_nVarRefIndex;

	sqvector< varref_t > m_Vars;
	sqvector< watch_t > m_LockedWatches;
	sqvector< breakpoint_t > m_Breakpoints;
	sqvector< datawatch_t > m_DataWatches;
	sqvector< classdef_t > m_ClassDefinitions;
	sqvector< SQWeakRef* > m_Threads;
	sqvector< frameid_t > m_FrameIDs;

	CFilePathMap m_FilePathMap;
	sqvector< script_t > m_Scripts;

	char *m_pScratch;
	int m_nScratchSize;

	CServerSocket m_Server;

public:
	char *ScratchPad( int size );
	char *ScratchPad() { return m_pScratch; }

public:
	SQDebugServer();

	void Attach( HSQUIRRELVM vm );
	bool ListenSocket( unsigned short port );
	void Shutdown();
	void DisconnectClient();
	void Frame();

	bool IsClientConnected() { return m_Server.IsClientConnected(); }
	bool IsListening() { return m_Server.IsListening(); }

private:
	void PrintLastServerMessage()
	{
	#ifdef SQUNICODE
		SQChar wcsFmt[128], wcsMsg[128];
		UTF8ToUnicode( wcsFmt, m_Server.m_pszLastMsgFmt, sizeof(wcsFmt) );
		UTF8ToUnicode( wcsMsg, m_Server.m_pszLastMsg, sizeof(wcsMsg) );
		PrintError( wcsFmt, wcsMsg );
	#else
		PrintError( m_Server.m_pszLastMsgFmt, m_Server.m_pszLastMsg );
	#endif
	}

	void Recv()
	{
		if ( !m_Server.Recv() )
		{
			PrintLastServerMessage();
		}
	}

	void Send( const char* buf, int len )
	{
		if ( !m_Server.Send( buf, len ) )
		{
			PrintLastServerMessage();
		}
	}

	static void _OnMessageReceived( void *dbg, char *ptr, int len )
	{
		((SQDebugServer*)dbg)->OnMessageReceived( ptr, len );
	}

	void OnMessageReceived( char *ptr, int len );

	void ProcessRequest( const json_table_t &table, int seq );
	void ProcessResponse( const json_table_t &table, int seq );
	void ProcessEvent( const json_table_t &table );

	void OnRequest_Initialize( const json_table_t &arguments, int seq );
	void OnRequest_SetBreakpoints( const json_table_t &arguments, int seq );
	void OnRequest_SetFunctionBreakpoints( const json_table_t &arguments, int seq );
	void OnRequest_SetExceptionBreakpoints( const json_table_t &arguments, int seq );
	void OnRequest_SetDataBreakpoints( const json_table_t &arguments, int seq );
	void OnRequest_DataBreakpointInfo( const json_table_t &arguments, int seq );
	void OnRequest_Evaluate( const json_table_t &arguments, int seq );
	void OnRequest_Scopes( const json_table_t &arguments, int seq );
	void OnRequest_Threads( int seq );
	void OnRequest_StackTrace( const json_table_t &arguments, int seq );
	void OnRequest_Variables( const json_table_t &arguments, int seq );
	void OnRequest_SetVariable( const json_table_t &arguments, int seq );
	void OnRequest_SetExpression( const json_table_t &arguments, int seq );
	void OnRequest_Disassemble( const json_table_t &arguments, int seq );
#ifdef SUPPORTS_RESTART_FRAME
	void OnRequest_RestartFrame( const json_table_t &arguments, int seq );
#endif
	void OnRequest_GotoTargets( const json_table_t &arguments, int seq );
	void OnRequest_Goto( const json_table_t &arguments, int seq );

private:
	bool AddBreakpoint( int line, const string_t &src,
			const string_t &condition, int hitsTarget, const string_t &logMessage, int id );
	bool AddFunctionBreakpoint( const string_t &func, const string_t &funcsrc,
			const string_t &condition, int hitsTarget, const string_t &logMessage, int id );

	breakpoint_t *GetBreakpoint( int line, const sqstring_t &src, int *id );
	breakpoint_t *GetFunctionBreakpoint( const sqstring_t &func, const sqstring_t &funcsrc, int *id );

	void FreeBreakpoint( breakpoint_t &bp );
	static inline bool HasCondition( const breakpoint_t *bp );
	bool CheckBreakpointCondition( breakpoint_t *bp, HSQUIRRELVM vm, int frame );
	void TracePoint( breakpoint_t *bp, HSQUIRRELVM vm, int frame );

	bool AddDataBreakpoint( string_t &dataId, const string_t &condition, int hitsTarget, int id );
	void CheckDataBreakpoints( HSQUIRRELVM vm );
	static void FreeDataWatch( datawatch_t &dw );

	inline void RemoveAllBreakpoints();
	inline void RemoveBreakpoints( const string_t &source );
	inline void RemoveFunctionBreakpoints();
	inline void RemoveDataBreakpoints();

private:
	static inline bool IsValidStackFrame( HSQUIRRELVM vm, int id );
	static inline int GetCurrentStackFrame( HSQUIRRELVM vm );
	static inline int GetStackBase( HSQUIRRELVM vm, int frameTarget );

	static SQWeakRef *GetWeakRef( HSQUIRRELVM vm ) { return GetWeakRef( vm, OT_THREAD ); }
	static SQWeakRef *GetWeakRef( SQRefCounted *obj, SQObjectType type ) { return obj->GetWeakRef( type ); }

	string_t GetValue( const SQObject &obj, int flags = 0 );
	string_t GetValue( int val, int flags = 0 )
	{
		SQObject obj;
		obj._type = OT_INTEGER;
		obj._unVal.nInteger = val;
		return GetValue( obj, flags );
	}

	int DescribeInstruction( SQInstruction *instr, SQFunctionProto *func, char *buf, int size );

private:
	bool GetObj( string_t &expression, bool identifierIsString, const SQObject &var, objref_t &out );
	bool GetObj( const string_t &expression, HSQUIRRELVM vm, int frame, objref_t &out );
	bool GetObj( string_t &expression, const varref_t *ref, objref_t &out );

	bool Get( const objref_t obj, SQObjectPtr &value );
	bool Set( const objref_t obj, const SQObjectPtr &value );

private:
	bool RunExpression( const string_t &expression, HSQUIRRELVM vm, int frame, SQObjectPtr &out,
			bool multiline = false );
	bool CompileIdentifier( const string_t &expression, const SQObject &env, SQObjectPtr &out );
	int EvalAndWriteExpr( char *buf, int size, string_t &expression, HSQUIRRELVM vm, int frame );

	bool CompileScript( const string_t &script, SQObjectPtr &ret );
	bool RunScript( HSQUIRRELVM vm, const string_t &script, const SQObject *env, SQObjectPtr &ret,
			bool multiline = false  );
	bool RunClosure( const SQObjectPtr &closure, const SQObject *env, SQObjectPtr &ret );
	bool RunClosure( const SQObjectPtr &closure,
			const SQObject *env, const SQObjectPtr &p1, SQObjectPtr &ret );
	bool RunClosure( const SQObjectPtr &closure,
			const SQObject *env, const SQObjectPtr &p1, const SQObjectPtr &p2, SQObjectPtr &ret );

private:
	script_t *GetScript( const string_t &source );
	void RemoveScripts();

public:
	void OnScriptCompile( const SQChar *script, SQInteger scriptlen, const SQChar *sourcename );

private:
	static bool ParseEvaluateName( const string_t &expression, HSQUIRRELVM vm, int frame, objref_t &out );
	static int ParseFormatSpecifiers( string_t &expression, char **ppComma = NULL );
	static bool ParseBinaryNumber( const string_t &value, SQObject &out );
	static inline bool ShouldParseEvaluateName( const string_t &expression );
	static inline bool ShouldPageArray( const SQObject &obj, unsigned int limit );

private:
	void InitEnv_GetVal( SQObjectPtr &env );
	void InitEnv_GetKey( SQObjectPtr &env );
	void SetEnvDelegate( SQObjectPtr &env, const SQObject &delegate );
	void ClearEnvDelegate( SQObjectPtr &env );
	static void SetEnvRoot( SQObjectPtr &env, const SQObject &root );
	void CacheLocals( HSQUIRRELVM vm, int frame, SQObjectPtr &env );

private:
	static inline bool ShouldIgnoreStackFrame( const SQVM::CallInfo &ci );
	int ConvertToFrameID( int threadId, int stackFrame );
	bool TranslateFrameID( int frameId, HSQUIRRELVM *thread, int *stackFrame );

	int ThreadToID( HSQUIRRELVM vm );
	HSQUIRRELVM ThreadFromID( int id );
	inline void RemoveThreads();

private:
	int ToVarRef( EVARREF type, HSQUIRRELVM vm, int stack );
	int ToVarRef( EVARREF type, const SQObject &obj, bool isWeakref = false );
	int ToVarRef( const SQObject &obj, bool isWeakref = false ) { return ToVarRef( VARREF_OBJ, obj, isWeakref ); }

	static inline void ConvertToWeakRef( varref_t &v );

	inline varref_t *FromVarRef( int i );
	inline void RemoveVarRefs( bool all );
	inline void RemoveLockedWatches();

	void Suspend();
	void Break( HSQUIRRELVM vm );
	void Continue( HSQUIRRELVM vm );

	classdef_t *FindClassDef( SQClass *base );
	void DefineClass( SQClass *target, SQTable *params );
	inline void RemoveClassDefs();

	sqstring_t PrintDisassembly( SQClosure *target );

private:
	void ErrorHandler( HSQUIRRELVM vm );
	void DebugHook( HSQUIRRELVM vm, SQInteger type,
			const SQChar *sourcename, SQInteger line, const SQChar *funcname );

	void OnSQPrint( HSQUIRRELVM vm, const SQChar *buf, int len );
	void OnSQError( HSQUIRRELVM vm, const SQChar *buf, int len );

	template < typename T >
	void SendEvent_OutputStdOut( const T &strOutput, const SQVM::CallInfo *ci );

public:
	static SQInteger SQDefineClass( HSQUIRRELVM vm );
	static SQInteger SQPrintDisassembly( HSQUIRRELVM vm );
	static SQInteger SQBreak( HSQUIRRELVM vm );

	static void SQPrint( HSQUIRRELVM vm, const SQChar *fmt, ... );
	static void SQError( HSQUIRRELVM vm, const SQChar *fmt, ... );

#ifndef CALL_DEFAULT_ERROR_HANDLER
	static void SQErrorAtFrame( HSQUIRRELVM vm, const SQVM::CallInfo *ci, const SQChar *fmt, ... );
	static void PrintVar( HSQUIRRELVM vm, const SQChar* name, const SQObjectPtr &obj );
	static void PrintStack( HSQUIRRELVM vm );
#endif

	static SQInteger SQErrorHandler( HSQUIRRELVM vm );
#ifdef NATIVE_DEBUG_HOOK
	static void SQDebugHook( HSQUIRRELVM vm, SQInteger type,
			const SQChar *sourcename, SQInteger line, const SQChar *funcname );
#else
	static SQInteger SQDebugHook( HSQUIRRELVM vm );
#endif
	static SQInteger SQRelease( SQUserPointer pDebugServer, SQInteger size );
};

SQDebugServer::SQDebugServer() :
	m_State( ThreadState::Running ),
	m_nStateCalls( 0 ),
	m_nCalls( 0 ),
	m_pRootVM( NULL ),
	m_pCurVM( NULL ),
	m_Print( NULL ),
	m_PrintError( NULL ),
	m_bBreakOnExceptions( 0 ),
	m_bIgnoreDebugHookGuard( 0 ),
	m_bDebugHookGuard( 0 ),
#if SQUIRREL_VERSION_NUMBER < 300
	m_bInDebugHook( 0 ),
	m_bExceptionPause( 0 ),
#endif
	m_bIgnoreExceptions( 0 ),
	m_pPausedThread( NULL ),
	m_Sequence( 1 ),
	m_nBreakpointIndex( 1 ),
	m_nVarRefIndex( 1 ),
	m_pScratch( NULL ),
	m_nScratchSize( 0 )
{
}

char *SQDebugServer::ScratchPad( int size )
{
	if ( m_pScratch )
	{
		if ( size <= m_nScratchSize )
			return m_pScratch;

		int oldsize = m_nScratchSize;
		m_nScratchSize = ( size + 7 ) & ~7;
		m_pScratch = (char*)sqdbg_realloc( m_pScratch, oldsize, m_nScratchSize );
		return m_pScratch;
	}

	m_nScratchSize = ( size + 7 ) & ~7;
	m_pScratch = (char*)sqdbg_malloc( m_nScratchSize );
	return m_pScratch;
}

void SQDebugServer::Attach( HSQUIRRELVM vm )
{
	if ( m_pRootVM )
	{
		if ( m_pRootVM == _thread(_ss(vm)->_root_vm) )
		{
			Print(_SC("(sqdbg) Debugger is already attached to this VM\n"));
		}
		else
		{
			Print(_SC("(sqdbg) Debugger is already attached to another VM\n"));
		}

		return;
	}

	m_pRootVM = _thread(_ss(vm)->_root_vm);
	m_pCurVM = vm;

	ThreadToID( m_pRootVM );

#if SQUIRREL_VERSION_NUMBER >= 300
	m_Print = sq_getprintfunc( m_pRootVM );
	m_PrintError = sq_geterrorfunc( m_pRootVM );
	sq_setprintfunc( m_pRootVM, SQPrint, SQError );
#else
	m_Print = sq_getprintfunc( m_pRootVM );
	m_PrintError = m_Print;
	sq_setprintfunc( m_pRootVM, SQPrint );
#endif

	Assert( m_Print && m_PrintError );

#ifdef NATIVE_DEBUG_HOOK
	sq_setnativedebughook( m_pRootVM, SQDebugHook );
#else
	sq_newclosure( m_pRootVM, &SQDebugHook, 0 );
	sq_setdebughook( m_pRootVM );
#endif

	m_ErrorHandler = m_pRootVM->_errorhandler;

	if ( sq_type(m_ErrorHandler) != OT_NULL )
		sq_addref( m_pRootVM, &m_ErrorHandler );

	sq_newclosure( m_pRootVM, &SQErrorHandler, 0 );
#ifdef CALL_DEFAULT_ERROR_HANDLER
	sq_setnativeclosurename( m_pRootVM, -1, _SC("sqdbg") );
#endif
	sq_seterrorhandler( m_pRootVM );

	sq_enabledebuginfo( m_pRootVM, 1 );

	SQString *cached = SQString::Create( _ss(m_pRootVM), _SC("sqdbg"), STRLEN("sqdbg") );
	m_sqstrDelegate = SQString::Create( _ss(m_pRootVM), _SC(KW_DELEGATE), STRLEN(KW_DELEGATE) );
	// 'this' keyword compiles in the temp env, add custom keyword to redirect
	// Having a local named '__this' will break this hack
	m_sqstrThis = SQString::Create( _ss(m_pRootVM), _SC(KW_THIS), STRLEN(KW_THIS) );

	__ObjAddRef( cached );

	InitEnv_GetVal( m_EnvGetVal );
	InitEnv_GetKey( m_EnvGetKey );

	sq_addref( m_pRootVM, &m_EnvGetVal );
	sq_addref( m_pRootVM, &m_EnvGetKey );

	{
		CStackCheck stackcheck( m_pRootVM );
		sq_pushroottable( m_pRootVM );

		sq_pushstring( m_pRootVM, _SC("sqdbg_define_class"), STRLEN("sqdbg_define_class") );
		sq_newclosure( m_pRootVM, &SQDebugServer::SQDefineClass, 0 );
		sq_setnativeclosurename( m_pRootVM, -1, _SC("sqdbg_define_class") );
		sq_setparamscheck( m_pRootVM, 3, _SC(".yt") );
		sq_newslot( m_pRootVM, -3, SQFalse );

		sq_pushstring( m_pRootVM, _SC("sqdbg_disassemble"), STRLEN("sqdbg_disassemble") );
		sq_newclosure( m_pRootVM, &SQDebugServer::SQPrintDisassembly, 0 );
		sq_setnativeclosurename( m_pRootVM, -1, _SC("sqdbg_disassemble") );
		sq_setparamscheck( m_pRootVM, 2, _SC(".c") );
		sq_newslot( m_pRootVM, -3, SQFalse );

		sq_pushstring( m_pRootVM, _SC("sqdbg_break"), STRLEN("sqdbg_break") );
		sq_newclosure( m_pRootVM, &SQDebugServer::SQBreak, 0 );
		sq_setnativeclosurename( m_pRootVM, -1, _SC("sqdbg_break") );
		sq_setparamscheck( m_pRootVM, 1, _SC(".") );
		sq_newslot( m_pRootVM, -3, SQFalse );

		sq_pop( m_pRootVM, 1 );
	}

	m_FrameIDs.reserve(8);
}

bool SQDebugServer::ListenSocket( unsigned short port )
{
	if ( IsListening() )
	{
		port = m_Server.GetServerPort();

		if ( port )
		{
			Print(_SC("(sqdbg) Socket already open on port %d\n"), port);
		}
		else
		{
			Print(_SC("(sqdbg) Socket already open\n"));
		}

		return true;
	}

	if ( !m_Server.ListenSocket( port ) )
	{
		PrintLastServerMessage();
		return false;
	}

	Print(_SC("(sqdbg) Listening for connections on port %d\n"), port);
	return true;
}

void SQDebugServer::Shutdown()
{
	if ( !m_pRootVM )
		return;

	Print(_SC("(sqdbg) Shutdown\n"));

	if ( IsClientConnected() )
	{
		m_Server.Execute< _OnMessageReceived >( this );

		DAP_START_EVENT( m_Sequence++, "terminated" );
		DAP_SEND();
	}

	m_Server.Shutdown();

	m_Sequence = 1;
	m_nBreakpointIndex = 1;
	m_nVarRefIndex = 1;

	m_State = ThreadState::Running;
	m_nStateCalls = 0;
	m_nCalls = 0;

	m_bIgnoreDebugHookGuard = false;
	m_bDebugHookGuard = false;
#if SQUIRREL_VERSION_NUMBER < 300
	m_bInDebugHook = false;
	m_bExceptionPause = false;
#endif
	m_pPausedThread = NULL;
	m_ReturnValue.Null();

	RemoveVarRefs( true );
	RemoveAllBreakpoints();
	RemoveLockedWatches();
	RemoveDataBreakpoints();

	RemoveThreads();
	RemoveClassDefs();
	RemoveScripts();

	m_FrameIDs.resize(0);

	sq_release( m_pRootVM, &m_EnvGetVal );
	sq_release( m_pRootVM, &m_EnvGetKey );

	m_EnvGetVal.Null();
	m_EnvGetKey.Null();

	if ( m_pScratch )
	{
		sqdbg_free( m_pScratch, m_nScratchSize );
		m_pScratch = NULL;
	}

	SQString *cached = SQString::Create( _ss(m_pRootVM), _SC("sqdbg"), STRLEN("sqdbg") );
	__ObjRelease( cached );
	m_sqstrDelegate.Null();
	m_sqstrThis.Null();

#if SQUIRREL_VERSION_NUMBER >= 300
	sq_setprintfunc( m_pRootVM, m_Print, m_PrintError );
#else
	sq_setprintfunc( m_pRootVM, m_Print );
#endif

	m_Print = m_PrintError = NULL;

#ifdef NATIVE_DEBUG_HOOK
	sq_setnativedebughook( m_pRootVM, NULL );
#else
	sq_pushnull( m_pRootVM );
	sq_setdebughook( m_pRootVM );
#endif

	sq_pushobject( m_pRootVM, m_ErrorHandler );
	sq_seterrorhandler( m_pRootVM );

	if ( sq_type(m_ErrorHandler) != OT_NULL )
	{
		sq_release( m_pRootVM, &m_ErrorHandler );
		m_ErrorHandler.Null();
	}

	sq_enabledebuginfo( m_pRootVM, 0 );
	sq_notifyallexceptions( m_pRootVM, 0 );

	m_pRootVM = m_pCurVM = NULL;
}

void SQDebugServer::DisconnectClient()
{
	if ( IsClientConnected() )
	{
		Print(_SC("(sqdbg) Client disconnected\n"));

		DAP_START_EVENT( m_Sequence++, "terminated" );
		DAP_SEND();
	}

	m_Sequence = 1;
	m_nBreakpointIndex = 1;
	m_nVarRefIndex = 1;

	m_bIgnoreDebugHookGuard = false;
	m_bDebugHookGuard = false;
#if SQUIRREL_VERSION_NUMBER < 300
	m_bInDebugHook = false;
	m_bExceptionPause = false;
#endif
	m_pPausedThread = NULL;
	m_ReturnValue.Null();

	RemoveVarRefs( true );
	RemoveAllBreakpoints();
	RemoveLockedWatches();
	RemoveDataBreakpoints();

	ClearEnvDelegate( m_EnvGetVal );
	ClearEnvDelegate( m_EnvGetKey );

	if ( m_pScratch )
	{
		sqdbg_free( m_pScratch, m_nScratchSize );
		m_pScratch = NULL;
	}

	m_Server.DisconnectClient();
}

void SQDebugServer::Frame()
{
	if ( m_Server.IsClientConnected() )
	{
		Recv();
		m_Server.Parse< DAP_ReadHeader >();
		m_Server.Execute< _OnMessageReceived >( this );
	}
	else
	{
		if ( m_Server.Listen() )
		{
			Print(_SC("(sqdbg) Client connected from " FMT_CSTR "\n"), m_Server.m_pszLastMsg);
		}
	}
}

#define GET_AND_VALIDATE( _base, _val ) \
		if ( !(_base).Get( #_val, &_val ) ) \
		{ \
			PrintError(_SC("(sqdbg) invalid message, could not find '" #_val "'\n")); \
			return; \
		}

void SQDebugServer::OnMessageReceived( char *ptr, int len )
{
	json_table_t table;
	JSONParser parser( ptr, len, &table );

	if ( parser.GetError() )
	{
		PrintError(_SC("(sqdbg) invalid DAP body : " FMT_CSTR "\n"), parser.GetError());
		Assert(!"invalid DAP body");
		return;
	}

	string_t type;
	table.GetString( "type", &type );

	if ( type.IsEqualTo( "request" ) )
	{
		int seq;
		GET_AND_VALIDATE( table, seq );

		ProcessRequest( table, seq );
	}
	else if ( type.IsEqualTo( "response" ) )
	{
		int request_seq;
		GET_AND_VALIDATE( table, request_seq );

		string_t command;
		table.GetString( "command", &command );

		PrintError(_SC("(sqdbg) Unrecognised response '" FMT_VCSTR "'\n"), STR_EXPAND(command));
		AssertMsgF( 0, "Unrecognised response '%s'", command.ptr );
	}
	else if ( type.IsEqualTo( "event" ) )
	{
		string_t event;
		table.GetString( "event", &event );

		PrintError(_SC("(sqdbg) Unrecognised event '" FMT_VCSTR "'\n"), STR_EXPAND(event));
		AssertMsgF( 0, "Unrecognised event '%s'", event.ptr );
	}
	else
	{
		PrintError(_SC("(sqdbg) invalid DAP type : '" FMT_VCSTR "'\n"), STR_EXPAND(type));
		Assert(!"invalid DAP type");
	}
}

void SQDebugServer::ProcessRequest( const json_table_t &table, int seq )
{
	string_t command;
	table.GetString( "command", &command );

	if ( command.IsEqualTo( "setBreakpoints" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_SetBreakpoints( *arguments, seq );
	}
	else if ( command.IsEqualTo( "setFunctionBreakpoints" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_SetFunctionBreakpoints( *arguments, seq );
	}
	else if ( command.IsEqualTo( "setExceptionBreakpoints" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_SetExceptionBreakpoints( *arguments, seq );
	}
	else if ( command.IsEqualTo( "setDataBreakpoints" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_SetDataBreakpoints( *arguments, seq );
	}
	else if ( command.IsEqualTo( "dataBreakpointInfo" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_DataBreakpointInfo( *arguments, seq );
	}
	else if ( command.IsEqualTo( "evaluate" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_Evaluate( *arguments, seq );
	}
	else if ( command.IsEqualTo( "scopes" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_Scopes( *arguments, seq );
	}
	else if ( command.IsEqualTo( "threads" ) )
	{
		OnRequest_Threads( seq );
	}
	else if ( command.IsEqualTo( "stackTrace" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_StackTrace( *arguments, seq );
	}
	else if ( command.IsEqualTo( "variables" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_Variables( *arguments, seq );
	}
	else if ( command.IsEqualTo( "setVariable" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_SetVariable( *arguments, seq );
	}
	else if ( command.IsEqualTo( "setExpression" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_SetExpression( *arguments, seq );
	}
	else if ( command.IsEqualTo( "setHitCount" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		int breakpointId, hitCount;
		arguments->GetInt( "breakpointId", &breakpointId );
		arguments->GetInt( "hitCount", &hitCount );

		if ( breakpointId > 0 && breakpointId < m_nBreakpointIndex )
		{
#define _check( vec, type ) \
			for ( unsigned int i = 0; i < vec.size(); i++ ) \
			{ \
				type &bp = vec[i]; \
				if ( bp.id == breakpointId ) \
				{ \
					Assert( bp.hitsTarget ); \
					bp.hits = hitCount; \
					DAP_START_RESPONSE( seq, "setHitCount" ); \
					DAP_SEND(); \
					return; \
				} \
			}

			_check( m_Breakpoints, breakpoint_t );
			_check( m_DataWatches, datawatch_t );
#undef _check
		}

		DAP_ERROR_RESPONSE( seq, "setHitCount" );
		DAP_ERROR_BODY( 0, "invalid breakpoint {id}", 1 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "id", GetValue( breakpointId ) );
		DAP_SEND();
	}
	else if ( command.IsEqualTo( "source" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		json_table_t *source;
		if ( arguments->GetTable( "source", &source ) )
		{
			string_t srcname;
			source->GetString( "name", &srcname );

			script_t *scr = GetScript( srcname );
			if ( scr )
			{
				DAP_START_RESPONSE( seq, "source" );
				DAP_SET_TABLE( body, 1 );
					body.SetStringNoCopy( "content", { scr->scriptptr, scr->scriptlen } );
				DAP_SEND();
				return;
			}
		}

		DAP_ERROR_RESPONSE( seq, "source" );
		DAP_SEND();
	}
	else if ( command.IsEqualTo( "disassemble" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_Disassemble( *arguments, seq );
	}
#ifdef SUPPORTS_RESTART_FRAME
	else if ( command.IsEqualTo( "restartFrame" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_RestartFrame( *arguments, seq );

		m_ReturnValue.Null();
	}
#endif
	else if ( command.IsEqualTo( "gotoTargets" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_GotoTargets( *arguments, seq );
	}
	else if ( command.IsEqualTo( "goto" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_Goto( *arguments, seq );

		m_ReturnValue.Null();
	}
	else if ( command.IsEqualTo( "next" ) )
	{
		DAP_START_RESPONSE( seq, "next" );
		DAP_SEND();

#if SQUIRREL_VERSION_NUMBER < 300
		if ( m_bExceptionPause )
		{
			Continue( m_pCurVM );
			return;
		}
#endif

		if ( m_State == ThreadState::Suspended )
		{
			m_State = ThreadState::NextLine;
			m_nStateCalls = m_nCalls;
		}

		m_ReturnValue.Null();
	}
	else if ( command.IsEqualTo( "stepIn" ) )
	{
		DAP_START_RESPONSE( seq, "stepIn" );
		DAP_SEND();

#if SQUIRREL_VERSION_NUMBER < 300
		if ( m_bExceptionPause )
		{
			Continue( m_pCurVM );
			return;
		}
#endif

		if ( m_State == ThreadState::Suspended )
		{
			m_State = ThreadState::StepIn;
		}

		m_ReturnValue.Null();
	}
	else if ( command.IsEqualTo( "stepOut" ) )
	{
		DAP_START_RESPONSE( seq, "stepOut" );
		DAP_SEND();

#if SQUIRREL_VERSION_NUMBER < 300
		if ( m_bExceptionPause )
		{
			Continue( m_pCurVM );
			return;
		}
#endif

		if ( m_State == ThreadState::Suspended )
		{
			m_State = ThreadState::StepOut;
			m_nStateCalls = m_nCalls;
		}

		m_ReturnValue.Null();
	}
	else if ( command.IsEqualTo( "continue" ) )
	{
		DAP_START_RESPONSE( seq, "continue" );
		DAP_SET_TABLE( body, 1 );
			body.SetBool( "allThreadsContinued", true );
		DAP_SEND();

		Continue( m_pCurVM );
	}
	else if ( command.IsEqualTo( "pause" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		int threadId;
		arguments->GetInt( "threadId", &threadId );

		HSQUIRRELVM vm = ThreadFromID( threadId );
		if ( vm )
		{
			DAP_START_RESPONSE( seq, "pause" );
			DAP_SEND();

			if ( m_State != ThreadState::Suspended )
			{
				m_pPausedThread = vm;
			}
		}
		else
		{
			DAP_ERROR_RESPONSE( seq, "pause" );
			DAP_ERROR_BODY( 0, "thread is dead", 0 );
			DAP_SEND();
		}
	}
	else if ( command.IsEqualTo( "attach" ) )
	{
		Print(_SC("(sqdbg) Client attached\n"));

		DAP_START_RESPONSE( seq, "attach" );
		DAP_SEND();

		DAP_START_EVENT( seq, "process" );
		DAP_SET_TABLE( body, 3 );
			body.SetString( "name", "" );
			body.SetString( "startMethod", "attach" );
			body.SetInt( "pointerSize", (int)sizeof(void*) );
		DAP_SEND();
	}
	else if ( command.IsEqualTo( "disconnect" ) || command.IsEqualTo( "terminate" ) )
	{
		DAP_START_RESPONSE( seq, command );
		DAP_SEND();

		DisconnectClient();
	}
	else if ( command.IsEqualTo( "initialize" ) )
	{
		json_table_t *arguments;
		GET_AND_VALIDATE( table, arguments );

		OnRequest_Initialize( *arguments, seq );
	}
	else if ( command.IsEqualTo( "configurationDone" ) )
	{
		DAP_START_RESPONSE( seq, "configurationDone" );
		DAP_SEND();
	}
	else
	{
		DAP_ERROR_RESPONSE( seq, command );
		DAP_ERROR_BODY( 0, "Unrecognised request '{command}'", 1 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "command", command );
		DAP_SEND();
		AssertMsgF( 0, "Unrecognised request '%s'", command.ptr );
	}
}

void SQDebugServer::OnScriptCompile( const SQChar *script, SQInteger scriptlen, const SQChar *sourcename )
{
	int sourcenamelen = scstrlen(sourcename);
#ifdef SOURCENAME_HAS_PATH
	for ( const SQChar *c = sourcename + sourcenamelen - 1; c > sourcename; c-- )
	{
		if ( *c == '/' || *c == '\\' )
		{
			c++;
			sourcenamelen = sourcename + sourcenamelen - c;
			sourcename = c;
			break;
		}
	}
#endif

#ifdef SQUNICODE
	Assert( UTF8Length( sourcename ) < 256 );
#endif

	stringbuf_t< 256 > source;
	source.len = scstombs( source.ptr, sizeof(source.ptr), const_cast< SQChar* >(sourcename), sourcenamelen );

	script_t *scr = GetScript( source );

#ifdef SQUNICODE
	Assert( script[scriptlen] == 0 );
	int scriptbufsize = ( UTF8Length( script ) + 7 ) & ~7;
#else
	int scriptbufsize = ( scriptlen + 7 ) & ~7;
#endif

	if ( !scr )
	{
		scr = &m_Scripts.push_back();
		scr->source.Copy( source );
		scr->scriptbufsize = scriptbufsize;
		scr->scriptptr = (char*)sqdbg_malloc( scr->scriptbufsize );
	}
	else
	{
		int oldsize = scr->scriptbufsize;
		if ( oldsize != scriptbufsize )
		{
			scr->scriptbufsize = scriptbufsize;
			scr->scriptptr = (char*)sqdbg_realloc( scr->scriptptr, oldsize, scr->scriptbufsize );
		}
	}

	scr->scriptlen = scstombs( scr->scriptptr, scr->scriptbufsize, const_cast< SQChar* >(script), scriptlen );
}

script_t *SQDebugServer::GetScript( const string_t &source )
{
	for ( unsigned int i = 0; i < m_Scripts.size(); i++ )
	{
		script_t &scr = m_Scripts[i];
		if ( scr.source.IsEqualTo( source ) )
		{
			return &scr;
		}
	}

	return NULL;
}

void SQDebugServer::RemoveScripts()
{
	for ( unsigned int i = 0; i < m_Scripts.size(); i++ )
	{
		script_t &scr = m_Scripts[i];
		scr.source.Free();
		sqdbg_free( scr.scriptptr, scr.scriptbufsize );
	}

	m_Scripts.resize(0);
}

void SQDebugServer::OnRequest_Initialize( const json_table_t &arguments, int seq )
{
	string_t clientID, clientName;
	arguments.GetString( "clientID", &clientID, "<unknown>" );
	arguments.GetString( "clientName", &clientName, "<unknown>" );

	if ( clientName.IsEqualTo( clientID ) )
	{
		Print(_SC("(sqdbg) Client initialised: " FMT_VCSTR "\n"),
				STR_EXPAND(clientName));
	}
	else
	{
		Print(_SC("(sqdbg) Client initialised: " FMT_VCSTR " (" FMT_VCSTR ")\n"),
				STR_EXPAND(clientName), STR_EXPAND(clientID));
	}

	DAP_START_RESPONSE( seq, "initialize" );
	DAP_SET_TABLE( body, 19 );
		body.SetBool( "supportsConfigurationDoneRequest", true );
		body.SetBool( "supportsFunctionBreakpoints", true );
		body.SetBool( "supportsConditionalBreakpoints", true );
		body.SetBool( "supportsHitConditionalBreakpoints", true );
		body.SetBool( "supportsEvaluateForHovers", true );
		json_array_t &exceptionBreakpointFilters = body.SetArray( "exceptionBreakpointFilters", 2 );
		{
			json_table_t &filter = exceptionBreakpointFilters.AppendTable(4);
			filter.SetString( "filter", "unhandled" );
			filter.SetString( "label", "Unhandled exceptions" );
			filter.SetString( "description", "Break on uncaught exceptions" );
			filter.SetBool( "default", true );
		}
		{
			json_table_t &filter = exceptionBreakpointFilters.AppendTable(3);
			filter.SetString( "filter", "all" );
			filter.SetString( "label", "All exceptions" );
			filter.SetString( "description", "Break on both caught and uncaught exceptions" );
		}
		body.SetBool( "supportsSetVariable", true );
#ifdef SUPPORTS_RESTART_FRAME
		body.SetBool( "supportsRestartFrame", true );
#endif
		body.SetBool( "supportsGotoTargetsRequest", true );
		body.SetBool( "supportsSetExpression", true );
		body.SetBool( "supportsSetHitCount", true );
		body.SetArray( "supportedChecksumAlgorithms" );
		body.SetBool( "supportsValueFormattingOptions", true );
		body.SetBool( "supportsDelayedStackTraceLoading", true );
		body.SetBool( "supportsLogPoints", true );
		body.SetBool( "supportsTerminateRequest", true );
		body.SetBool( "supportsDataBreakpoints", true );
		body.SetBool( "supportsDisassembleRequest", true );
		body.SetBool( "supportsClipboardContext", true );
	DAP_SEND();

	DAP_START_EVENT( seq, "initialized" );
	DAP_SEND();
}

void SQDebugServer::OnRequest_SetBreakpoints( const json_table_t &arguments, int seq )
{
	json_array_t *breakpoints;
	json_table_t *source;

	arguments.GetArray( "breakpoints", &breakpoints );
	arguments.GetTable( "source", &source );

	string_t srcname, srcpath;
	source->GetString( "name", &srcname );
	source->GetString( "path", &srcpath );

	if ( !srcname.IsEmpty() && !srcpath.IsEmpty() )
		m_FilePathMap.Add( srcname, srcpath );

	RemoveBreakpoints( srcname );

	for ( unsigned int i = 0; i < breakpoints->size(); i++ )
	{
		Assert( breakpoints->Get(i)->type & JSON_TABLE );
		json_table_t &bp = *breakpoints->Get(i)->_t;

		int line, hitsTarget = 0;
		string_t condition, hitCondition, logMessage;

		bp.GetInt( "line", &line );
		bp.GetString( "condition", &condition );
		bp.GetString( "logMessage", &logMessage );

		if ( bp.GetString( "hitCondition", &hitCondition ) )
		{
			if ( !hitCondition.StartsWith("0x") )
			{
				atoi( hitCondition, &hitsTarget );
			}
			else
			{
				atox( hitCondition, &hitsTarget );
			}
		}

		int id = m_nBreakpointIndex++;
		bp.SetInt( "id", id );

		bool verified = AddBreakpoint( line, srcname, condition, hitsTarget, logMessage, id );
		bp.SetBool( "verified", verified );

		if ( verified )
		{
			bp.SetInt( "line", line );
		}
		else
		{
			bp.SetString( "reason", "failed" );
			bp.SetString( "message", GetValue( m_pCurVM->_lasterror, NO_QUOT ) );
		}
	}

	DAP_START_RESPONSE( seq, "setBreakpoints" );
	DAP_SET_TABLE( body, 1 );
		body.SetArray( "breakpoints", *breakpoints );
	DAP_SEND();
}

void SQDebugServer::OnRequest_SetFunctionBreakpoints( const json_table_t &arguments, int seq )
{
	json_array_t *breakpoints;
	arguments.GetArray( "breakpoints", &breakpoints );

	RemoveFunctionBreakpoints();

	for ( unsigned int i = 0; i < breakpoints->size(); i++ )
	{
		Assert( breakpoints->Get(i)->type & JSON_TABLE );
		json_table_t &bp = *breakpoints->Get(i)->_t;

		int hitsTarget = 0;
		string_t name, condition, hitCondition, logMessage;

		bp.GetString( "name", &name );
		bp.GetString( "condition", &condition );
		bp.GetString( "logMessage", &logMessage );

		if ( bp.GetString( "hitCondition", &hitCondition ) )
		{
			if ( !hitCondition.StartsWith("0x") )
			{
				atoi( hitCondition, &hitsTarget );
			}
			else
			{
				atox( hitCondition, &hitsTarget );
			}
		}

		string_t funcsrc( (char*)0, 0 );

		// function source: funcname,filename
		for ( int j = name.len - 1; j > 1; j-- )
		{
			if ( name.ptr[j] == ',' )
			{
				funcsrc.ptr = name.ptr + j + 1;
				funcsrc.len = name.len - j - 1;
				name.len = j;

			#ifdef SQUNICODE
				// UTF8ToUnicode expects terminated strings
				name.ptr[name.len] = 0;
				funcsrc.ptr[funcsrc.len] = 0;
			#endif
				break;
			}
		}

		// anonymous function
		if ( name.StartsWith("()") )
			name.Assign("");

		int id = m_nBreakpointIndex++;
		bp.SetInt( "id", id );

		bool verified = AddFunctionBreakpoint( name, funcsrc, condition, hitsTarget, logMessage, id );
		bp.SetBool( "verified", verified );

		if ( !verified )
		{
			bp.SetString( "reason", "failed" );
			bp.SetString( "message", GetValue( m_pCurVM->_lasterror, NO_QUOT ) );
		}
	}

	DAP_START_RESPONSE( seq, "setFunctionBreakpoints" );
	DAP_SET_TABLE( body, 1 );
		body.SetArray( "breakpoints", *breakpoints );
	DAP_SEND();
}

void SQDebugServer::OnRequest_SetExceptionBreakpoints( const json_table_t &arguments, int seq )
{
	bool bCaught = false, bUncaught = false;

	json_array_t *filters;
	arguments.GetArray( "filters", &filters );

	for ( unsigned int i = 0; i < filters->size(); i++ )
	{
		Assert( filters->Get(i)->type & JSON_STRING );
		const string_t &filter = filters->Get(i)->_s;

		if ( filter.IsEqualTo( "unhandled" ) )
		{
			bUncaught = true;
		}
		else if ( filter.IsEqualTo( "all" ) )
		{
			bCaught = true;
		}
	}

	if ( filters->size() == 0 )
	{
		m_bBreakOnExceptions = false;
		sq_notifyallexceptions( m_pRootVM, 0 );
	}
	else
	{
		m_bBreakOnExceptions = true;

		if ( bCaught )
		{
			sq_notifyallexceptions( m_pRootVM, 1 );
		}
		else
		{
			Assert( bUncaught );
			(void)bUncaught;
			sq_notifyallexceptions( m_pRootVM, 0 );
		}
	}

	DAP_START_RESPONSE( seq, "setExceptionBreakpoints" );
	DAP_SEND();
}

void SQDebugServer::OnRequest_SetDataBreakpoints( const json_table_t &arguments, int seq )
{
	json_array_t *breakpoints;
	arguments.GetArray( "breakpoints", &breakpoints );

	RemoveDataBreakpoints();

	for ( unsigned int i = 0; i < breakpoints->size(); i++ )
	{
		Assert( breakpoints->Get(i)->type & JSON_TABLE );
		json_table_t &bp = *breakpoints->Get(i)->_t;

		int hitsTarget = 0;
		string_t dataId, condition, hitCondition;

		bp.GetString( "dataId", &dataId );
		bp.GetString( "condition", &condition );

		if ( bp.GetString( "hitCondition", &hitCondition ) )
		{
			if ( !hitCondition.StartsWith("0x") )
			{
				atoi( hitCondition, &hitsTarget );
			}
			else
			{
				atox( hitCondition, &hitsTarget );
			}
		}

		int id = m_nBreakpointIndex++;
		bp.SetInt( "id", id );

		bool verified = AddDataBreakpoint( dataId, condition, hitsTarget, id );
		bp.SetBool( "verified", verified );

		if ( !verified )
		{
			bp.SetString( "reason", "failed" );
			bp.SetString( "message", "" );
		}
	}

	DAP_START_RESPONSE( seq, "setDataBreakpoints" );
	DAP_SET_TABLE( body, 1 );
		body.SetArray( "breakpoints", *breakpoints );
	DAP_SEND();
}

void SQDebugServer::OnRequest_DataBreakpointInfo( const json_table_t &arguments, int seq )
{
	int variablesReference, frameId;
	string_t name;

	arguments.GetString( "name", &name );
	arguments.GetInt( "variablesReference", &variablesReference );
	arguments.GetInt( "frameId", &frameId );

	if ( variablesReference )
	{
		objref_t obj;
		varref_t *ref = FromVarRef( variablesReference );

		// don't modify name in UndoEscape
		Assert( name.len < 256 );
		stringbuf_t< 256 > tmpbuf;
		tmpbuf.Puts( name );
		tmpbuf.Term();
		string_t tmp = tmpbuf;

		if ( !ref || ref->type != VARREF_OBJ ||
				!GetObj( tmp, ref, obj ) )
		{
			DAP_START_RESPONSE( seq, "dataBreakpointInfo" );
			DAP_SET_TABLE( body, 2 );
				body.SetNull( "dataId" );
				body.SetString( "description", "" );
			DAP_SEND();
			return;
		}

		DAP_START_RESPONSE( seq, "dataBreakpointInfo" );
		DAP_SET_TABLE( body, 3 );
			{
				// 1:varref:name
				stringbuf_t< 256 > buf;
				buf.Put('1');
				buf.Put(':');
				buf.PutInt( variablesReference );
				buf.Put(':');
				buf.Puts( name );
				buf.Term();

				body.SetString( "dataId", buf );
			}
			{
				stringbuf_t< 256 > buf;
				buf.Put('[');
				buf.PutHex( _rawval( ref->GetVar() ) );
				buf.Put(' ');
				buf.Puts( GetType( ref->GetVar() ) );
				buf.Put(']');
				buf.Put('-');
				buf.Put('>');
				buf.Puts( name );
				buf.Term();

				body.SetString( "description", buf );
			}
			body.SetArray( "accessTypes", 1 ).Append( "write" );
		DAP_SEND();
	}
	else
	{
		DAP_START_RESPONSE( seq, "dataBreakpointInfo" );
		DAP_SET_TABLE( body, 2 );
			body.SetNull( "dataId" );
			body.SetString( "description", "expression data breakpoints not implemented" );
		DAP_SEND();
	}
}

bool SQDebugServer::AddDataBreakpoint( string_t &dataId, const string_t &strCondition, int hitsTarget, int id )
{
	string_t index, container, name;

	index.ptr = dataId.ptr;
	char *pEnd = strchr( index.ptr, ':' );

	if ( !pEnd )
		return false;

	index.len = pEnd - index.ptr;

	container.ptr = pEnd + 1;
	pEnd = strchr( pEnd + 1, ':' );

	if ( !pEnd )
		return false;

	container.len = pEnd - container.ptr;

	name.ptr = pEnd + 1;
	name.len = ( dataId.ptr + dataId.len ) - name.ptr;

	if ( index.ptr[0] == '1' )
	{
		int variablesReference;

		if ( !atoi( container, &variablesReference ) )
			return false;

		objref_t obj;
		varref_t *ref = FromVarRef( variablesReference );

		// don't modify name in UndoEscape
		Assert( name.len < 256 );
		stringbuf_t< 256 > tmpbuf;
		tmpbuf.Puts( name );
		tmpbuf.Term();
		string_t tmp = tmpbuf;

		if ( !ref || ref->type != VARREF_OBJ ||
				!GetObj( tmp, ref, obj ) )
			return false;

		// HACKHACK: Convert array index ptr
		if ( obj.type == objref_t::PTR && sq_type( ref->GetVar() ) == OT_ARRAY )
			obj.type = objref_t::ARRAY;

		Assert( obj.type != objref_t::PTR );

		SQObjectPtr value;

		if ( !Get( obj, value ) )
			return false;

		ConvertToWeakRef( *ref );

		// Duplicate?
		for ( int i = m_DataWatches.size(); i--; )
		{
			const datawatch_t &dw = m_DataWatches[i];
			if ( dw.container == ref->obj.weakref &&
					dw.obj.src.pRefCounted == obj.src.pRefCounted &&
					dw.obj.type == obj.type &&
					_rawval(dw.obj.key) == _rawval(obj.key) &&
					sq_type(dw.obj.key) == sq_type(obj.key) )
				return false;
		}

		int hasCondition = 0;
		SQObjectPtr condition;

		if ( !strCondition.IsEmpty() )
		{
			if ( !RunScript( m_pRootVM, strCondition, NULL, condition ) )
				return false;

			hasCondition = 1;
		}

		datawatch_t &dw = m_DataWatches.push_back();
		memset( &dw, 0, sizeof(datawatch_t) );
		dw.id = id;
		dw.name.Copy( name );
		dw.hitsTarget = hitsTarget;
		dw.container = ref->obj.weakref;
		__ObjAddRef( dw.container );
		dw.obj = obj;
		dw.oldvalue = value;

		if ( hasCondition )
		{
			dw.hasCondition = 1;
			dw.condition = condition;
		}

		return true;
	}

	return false;
}

void SQDebugServer::CheckDataBreakpoints( HSQUIRRELVM vm )
{
	for ( int i = m_DataWatches.size(); i--; )
	{
		datawatch_t &dw = m_DataWatches[i];

		if ( sq_type(dw.container->_obj) == OT_NULL )
		{
			DAP_START_EVENT( m_Sequence++, "breakpoint" );
			DAP_SET_TABLE( body, 2 );
				body.SetString( "reason", "removed" );
				json_table_t &bp = body.SetTable( "breakpoint", 2 );
				bp.SetInt( "id", dw.id );
				bp.SetBool( "verified", false );
			DAP_SEND();

			FreeDataWatch( dw );
			m_DataWatches.remove(i);
			continue;
		}

		SQObjectPtr value;

		if ( Get( dw.obj, value ) )
		{
			if ( _rawval(dw.oldvalue) != _rawval(value) )
			{
				SQObject oldvalue = dw.oldvalue;
				dw.oldvalue = value;

				if ( dw.hasCondition &&
						( _rawval(value) != _rawval(dw.condition) || sq_type(value) != sq_type(dw.condition) ) )
					continue;

				if ( dw.hitsTarget )
				{
					if ( ++dw.hits < dw.hitsTarget )
						continue;

					dw.hits = 0;
				}

				stringbuf_t< 256 > buf;
				buf.Put('[');
				buf.PutHex( _rawval( dw.container->_obj ) );
				buf.Put(' ');
				buf.Puts( GetType( dw.container->_obj ) );
				buf.Put(']');
				buf.Put('-');
				buf.Put('>');
				buf.Puts( dw.name );
				buf.Puts(" changed (");
				buf.Puts( GetValue( oldvalue ) );
				buf.Puts(")->(");
				buf.Puts( GetValue( value ) );
				buf.Put(')');
				buf.Term();

				SQPrint( vm, _SC("(sqdbg) Data breakpoint hit: " FMT_CSTR "\n"), buf.ptr );

				m_BreakReason.reason = BreakReason::DataBreakpoint;
				m_BreakReason.text.Assign( buf );
				m_BreakReason.id = dw.id;
				Break( vm );
			}
		}
		else
		{
			DAP_START_EVENT( m_Sequence++, "breakpoint" );
			DAP_SET_TABLE( body, 2 );
				body.SetString( "reason", "removed" );
				json_table_t &bp = body.SetTable( "breakpoint", 2 );
				bp.SetInt( "id", dw.id );
				bp.SetBool( "verified", false );
			DAP_SEND();

			stringbuf_t< 256 > buf;
			buf.Put('[');
			buf.PutHex( _rawval( dw.container->_obj ) );
			buf.Put(' ');
			buf.Puts( GetType( dw.container->_obj ) );
			buf.Put(']');
			buf.Put('-');
			buf.Put('>');
			buf.Puts( dw.name );
			buf.Puts( " was removed" );
			buf.Term();

			SQPrint( vm, _SC("(sqdbg) Data breakpoint hit: " FMT_CSTR "\n"), buf.ptr );

			m_BreakReason.reason = BreakReason::DataBreakpoint;
			m_BreakReason.text.Assign( buf );
			m_BreakReason.id = dw.id;
			Break( vm );

			FreeDataWatch( dw );
			m_DataWatches.remove(i);
		}
	}
}

void SQDebugServer::FreeDataWatch( datawatch_t &dw )
{
	if ( dw.container )
		__ObjRelease( dw.container );

	dw.name.Free();
}

void SQDebugServer::RemoveDataBreakpoints()
{
	for ( unsigned int i = 0; i < m_DataWatches.size(); i++ )
		FreeDataWatch( m_DataWatches[i] );

	m_DataWatches.resize(0);
}

static inline int CountEscapes( const SQChar *src, SQInteger len )
{
	const SQChar *end = src + len;
	int count = 0;

	do
	{
		switch ( *src++ )
		{
			case '\"': case '\\': case '\b':
			case '\f': case '\n': case '\r': case '\t':
				count++;
		}
	}
	while ( src < end );

	return count;
}

static char *Escape( char *dst, int len, int size )
{
	if ( size < len )
		len = size;

	char *memEnd = dst + size;
	char *strEnd = dst + len;

	do
	{
		switch ( *dst++ )
		{
			case '\"': case '\\': case '\b':
			case '\f': case '\n': case '\r': case '\t':
			{
				if ( dst >= memEnd )
				{
					dst[-1] = '\\';
					break;
				}

				memmove( dst, dst - 1, strEnd - ( dst - 1 ) );
				dst[-1] = '\\';

				switch ( *dst )
				{
					case '\b': *dst = 'b'; break;
					case '\f': *dst = 'f'; break;
					case '\n': *dst = 'n'; break;
					case '\r': *dst = 'r'; break;
					case '\t': *dst = 't'; break;
				}

				strEnd++;
				dst++;

				if ( strEnd > memEnd )
					strEnd = memEnd;
			}
		}
	}
	while ( dst < strEnd );

	return strEnd;
}

static void UndoEscape( char *dst, int *len )
{
	char *end = dst + *len;

	do
	{
		if ( *dst++ == '\\' )
		{
			switch ( *dst )
			{
				case '\"': dst[-1] = '\"'; break;
				case 'b': dst[-1] = '\b'; break;
				case 'f': dst[-1] = '\f'; break;
				case 'n': dst[-1] = '\n'; break;
				case 'r': dst[-1] = '\r'; break;
				case 't': dst[-1] = '\t'; break;
			}

			memmove( dst, dst + 1, end - dst );
			end--;
			(*len)--;
		}
	}
	while ( dst < end );
}

static inline string_t SpecialFloatValue( SQFloat val )
{
#ifdef SQUSEDOUBLE
	if ( val == DBL_MAX )
	{
		return "DBL_MAX";
	}
	if ( val == DBL_MIN )
	{
		return "DBL_MIN";
	}
	if ( val == DBL_EPSILON )
	{
		return "DBL_EPSILON";
	}
#endif
	if ( val == FLT_MAX )
	{
		return "FLT_MAX";
	}
	if ( val == FLT_MIN )
	{
		return "FLT_MIN";
	}
	if ( val == FLT_EPSILON )
	{
		return "FLT_EPSILON";
	}
	return { (char*)NULL, 0 };
}

string_t SQDebugServer::GetValue( const SQObject &obj, int flags )
{
	switch ( obj._type )
	{
		case OT_STRING:
		{
			if ( !( flags & NO_QUOT ) )
			{
				int size;

				if ( !( flags & CLAMP ) )
				{
					int escapes = CountEscapes( _string(obj)->_val, _string(obj)->_len );
			#ifdef SQUNICODE
					size = UTF8Length( _string(obj)->_val ) + 2 + escapes;
			#else
					size = _string(obj)->_len + 2 + escapes;
			#endif
				}
				else
				{
					size = 64;
				}

				char *buf = ScratchPad( size );
				buf[0] = '\"';
				int len = scstombs( buf + 1, size-2, _string(obj)->_val, _string(obj)->_len );
				len = 1 + Escape( buf + 1, len, size - 2 ) - buf;
				buf[ len - 1 ] = '\"';

				return { buf, len };
			}
			else
			{
			#ifdef SQUNICODE
				const int size = UTF8Length( _string(obj)->_val );
				char *buf = ScratchPad( size );
				return { buf, UnicodeToUTF8( buf, _string(obj)->_val, size ) };
			#else
				return _string(obj);
			#endif
			}
		}
		case OT_FLOAT:
		{
			if ( flags & DEC )
			{
				const int size = FMT_INT_LEN + 1;
				char *buf = ScratchPad( size );
				int len = printint( buf, size, _integer(obj) );
				return { buf, len };
			}
			else if ( flags & BIN )
			{
				const int len = 2 + ( sizeof( SQFloat ) << 3 );
				char *buf = ScratchPad( len + 1 );

				char *c = buf;
				*c++ = '0';
				*c++ = 'b';
				for ( int i = ( sizeof( SQFloat ) << 3 ); i--; )
					*c++ = '0' + ( ( _integer(obj) & ( (SQUnsignedInteger)1 << i ) ) != 0 );
				*c = 0;

				return { buf, len };
			}
			else
			{
				string_t val = SpecialFloatValue( _float(obj) );

				if ( !val.ptr )
				{
					const int size = FMT_FLT_LEN + 1;
					val.ptr = ScratchPad( size );
					val.len = snprintf( val.ptr, size, "%f", _float(obj) );
				}

				return val;
			}
		}
		case OT_INTEGER:
		{
			if ( flags & HEX )
			{
				const int size = FMT_PTR_LEN + 1;
				char *buf = ScratchPad( size );
				int len = printhex( buf, size, (SQUnsignedInteger)_integer(obj), false );
				return { buf, len };
			}
			else if ( flags & BIN )
			{
				const int len = 2 + ( sizeof( SQInteger ) << 3 );
				char *buf = ScratchPad( len + 1 );

				char *c = buf;
				*c++ = '0';
				*c++ = 'b';
				for ( int i = ( sizeof( SQInteger ) << 3 ); i--; )
					*c++ = '0' + ( ( _integer(obj) & ( (SQUnsignedInteger)1 << i ) ) != 0 );
				*c = 0;

				return { buf, len };
			}
			else if ( flags & FLT )
			{
				((SQObject&)obj)._type = OT_FLOAT;
				return GetValue( obj, flags );
			}
			else
			{
				const int size = FMT_INT_LEN + 1;
				char *buf = ScratchPad( size );
				int len = printint( buf, size, _integer(obj) );
				return { buf, len };
			}
		}
		case OT_BOOL:
		{
			if ( _integer(obj) )
				return "true";

			return "false";
		}
		case OT_NULL:
		{
			return "null";
		}
		case OT_ARRAY:
		{
			const int size = STRLEN(" {size = }") + FMT_PTR_LEN + FMT_INT_LEN + 1;
			char *buf = ScratchPad( size );
			int len = snprintf( buf, size, FMT_PTR " {size = " FMT_INT "}",
					_rawval(obj), _array(obj)->Size() );

			return { buf, len };
		}
		case OT_TABLE:
		{
			const int size = STRLEN(" {size = }") + FMT_PTR_LEN + FMT_INT_LEN + 1;
			char *buf = ScratchPad( size );
			int len = snprintf( buf, size, FMT_PTR " {size = " FMT_INT "}",
					_rawval(obj), _table(obj)->CountUsed() );

			return { buf, len };
		}
		case OT_INSTANCE:
		{
			Assert( _instance(obj)->_class );
			SQClass *base = _instance(obj)->_class;
			const classdef_t *def = FindClassDef( base );

			if ( def )
			{
				if ( sq_type(def->value) == OT_NULL  )
				{
					// Check hierarchy
					while ( ( base = base->_base ) != NULL )
					{
						if ( ( def = FindClassDef( base ) ) != NULL && sq_type(def->value) != OT_NULL  )
						{
							goto getinstancevalue;
						}
					}
				}
				else
				{
getinstancevalue:
					SQObjectPtr res;
					if ( RunClosure( def->value, &obj, res ) && sq_type(res) == OT_STRING )
					{
						if ( !(flags & PURE) )
						{
							const int size = 1024;
							char *buf = ScratchPad( size );
							int len = snprintf( buf, size, FMT_PTR " {" FMT_VSTR "}",
									_rawval(obj),
									min( (int)_string(res)->_len, size - FMT_PTR_LEN - 3 ), _string(res)->_val );

							return { buf, len };
						}
						else
						{
						#ifdef SQUNICODE
							const int size = UTF8Length( _string(res)->_val );
							char *buf = ScratchPad( size );
							return { buf, UnicodeToUTF8( buf, _string(res)->_val, size ) };
						#else
							return _string(res);
						#endif
						}
					}
				}
			}

			goto default_label;
		}
		case OT_CLASS:
		{
			SQClass *pClass = _class(obj);
			const classdef_t *def = FindClassDef( pClass );

			if ( def && def->name.ptr )
			{
				if ( !(flags & PURE) )
				{
					return def->name;
				}
				else
				{
					Assert( def->name.len >= FMT_PTR_LEN + 1 );
					return { def->name.ptr + FMT_PTR_LEN + 1, def->name.len - FMT_PTR_LEN - 1 };
				}
			}

			goto default_label;
		}
		default:
		default_label:
		{
			const int size = FMT_PTR_LEN + 1;
			char *buf = ScratchPad( size );
			int len = printhex( buf, size, _rawval(obj) );
			return { buf, len };
		}
	}
}

static inline bool IsJumpOp( SQInstruction *instr )
{
	return ( instr->op == _OP_JMP ||
#if SQUIRREL_VERSION_NUMBER >= 300
			instr->op == _OP_JCMP ||
#else
			instr->op == _OP_JNZ ||
#endif
			instr->op == _OP_JZ );
}

// Line ops are ignored in disassembly, but jump ops account for them.
// Count out all line ops in the jump
// Doing this for setting instructions adds too much complexity that is not worth the effort.
static int DeduceJumpCount( SQInstruction *instr )
{
	Assert( IsJumpOp( instr ) );

	int arg1 = instr->_arg1;
	int sign = ( arg1 < 0 );
	if ( sign )
		arg1 = -arg1;

	for ( SQInstruction *ip = instr + instr->_arg1; ip != instr; ip += sign ? 1 : -1 )
	{
		if ( ip->op == _OP_LINE )
			arg1--;
	}

	if ( sign )
		arg1 = -arg1;

	return arg1;
}

#define STRNCPY( dst, size, StrLiteral )\
	min( (int)STRLEN(StrLiteral), (size) ); memcpy( (dst), (StrLiteral), min( (int)STRLEN(StrLiteral), (size) ) )

int SQDebugServer::DescribeInstruction( SQInstruction *instr, SQFunctionProto *func, char *buf, int size )
{
#if SQUIRREL_VERSION_NUMBER < 212
	return 0;
#else
	int len = g_InstructionName[ instr->op ].len;
	memcpy( buf, g_InstructionName[ instr->op ].ptr, len );
	buf[len++] = ' ';
	buf += len;
	size -= len;

	switch ( instr->op )
	{
		case _OP_GETK:
		case _OP_PREPCALLK:
		case _OP_LOAD:
		{
			string_t v = GetValue( func->_literals[instr->_arg1], CLAMP );
			int l = min( size, v.len );
			memcpy( buf, v.ptr, l );
			len += l;
			break;
		}
		case _OP_DLOAD:
		{
			{
				string_t v = GetValue( func->_literals[instr->_arg1], CLAMP );

				int l = min( size-1, v.len );
				memcpy( buf, v.ptr, l );
				len += l;

				buf[l] = ',';
				len++;

				buf += l+1;
				size -= len;
			}
			{
				string_t v = GetValue( func->_literals[instr->_arg3], CLAMP );

				int l = min( size, v.len );
				memcpy( buf, v.ptr, l );
				len += l;
			}

			break;
		}
		case _OP_LOADINT:
		case _OP_LOADNULLS:
		{
			len += printint( buf, size, instr->_arg1 );
			break;
		}
		case _OP_LOADFLOAT:
		{
			string_t val = SpecialFloatValue( *(SQFloat*)&instr->_arg1 );

			if ( !val.ptr )
			{
				int l = snprintf( buf, size, "%f", *(SQFloat*)&instr->_arg1 );
				len += clamp( l, 0, size-1 );
			}
			else
			{
				Assert( size > val.len );
				memcpy( buf, val.ptr, val.len );
				len += val.len;
			}

			break;
		}
		case _OP_LOADBOOL:
		{
			if ( instr->_arg1 )
			{
				len += STRNCPY( buf, size, "true" );
			}
			else
			{
				len += STRNCPY( buf, size, "false" );
			}
			break;
		}
		case _OP_JMP:
		case _OP_JZ:
#if SQUIRREL_VERSION_NUMBER >= 300
		case _OP_JCMP:
#else
		case _OP_JNZ:
#endif
		{
			len += printint( buf, size, DeduceJumpCount( instr ) );
			break;
		}
		case _OP_CMP:
		{
			switch ( instr->_arg3 )
			{
				case CMP_G:		len += STRNCPY( buf, size, ">" ); break;
				case CMP_GE:	len += STRNCPY( buf, size, ">=" ); break;
				case CMP_L:		len += STRNCPY( buf, size, "<" ); break;
				case CMP_LE:	len += STRNCPY( buf, size, "<=" ); break;
#if SQUIRREL_VERSION_NUMBER >= 300
				case CMP_3W:	len += STRNCPY( buf, size, "<=>" ); break;
#endif
				default: UNREACHABLE();
			}
			break;
		}
		case _OP_BITW:
		{
			switch ( instr->_arg3 )
			{
				case BW_AND:		len += STRNCPY( buf, size, "&" ); break;
				case BW_OR:			len += STRNCPY( buf, size, "|" ); break;
				case BW_XOR:		len += STRNCPY( buf, size, "^" ); break;
				case BW_SHIFTL:		len += STRNCPY( buf, size, "<<" ); break;
				case BW_SHIFTR:		len += STRNCPY( buf, size, ">>" ); break;
				case BW_USHIFTR:	len += STRNCPY( buf, size, ">>>" ); break;
				default: UNREACHABLE();
			}
			break;
		}
		case _OP_COMPARITH:
#if SQUIRREL_VERSION_NUMBER < 300
		case _OP_COMPARITHL:
		case _OP_ARITH:
#endif
		{
			buf[0] = instr->_arg3;
			len++;
			break;
		}
#if SQUIRREL_VERSION_NUMBER >= 300
		case _OP_SETOUTER:
		case _OP_GETOUTER:
#else
		case _OP_LOADFREEVAR:
#endif
		{
			buf[0] = '[';

			SQString *v = _string( func->_outervalues[instr->_arg1]._name );
			int l = scstombs( buf + 1, size-2, v->_val, v->_len );
			len += l + 2;

			buf[l+1] = ']';
			break;
		}
#if SQUIRREL_VERSION_NUMBER >= 300
		case _OP_APPENDARRAY:
		{
			switch ( instr->_arg2 )
			{
				case AAT_INT:
				{
					len += printint( buf, size, instr->_arg1 );
					break;
				}
				case AAT_FLOAT:
				{
					string_t val = SpecialFloatValue( *(SQFloat*)&instr->_arg1 );

					if ( !val.ptr )
					{
						int l = snprintf( buf, size, "%f", *(SQFloat*)&instr->_arg1 );
						len += clamp( l, 0, size-1 );
					}
					else
					{
						Assert( size > val.len );
						memcpy( buf, val.ptr, val.len );
						len += val.len;
					}

					break;
				}
				case AAT_BOOL:
				{
					if ( instr->_arg1 )
					{
						len += STRNCPY( buf, size, "true" );
					}
					else
					{
						len += STRNCPY( buf, size, "false" );
					}
					break;
				}
				case AAT_LITERAL:
				{
					string_t v = GetValue( func->_literals[instr->_arg1], CLAMP );
					int l = min( size, v.len );
					memcpy( buf, v.ptr, l );
					len += l;
					break;
				}
			}
			len = min( size, len );
			break;
		}
		case _OP_NEWOBJ:
		{
			switch ( instr->_arg3 )
			{
				case NOT_TABLE: len += STRNCPY( buf, size, "TABLE" ); break;
				case NOT_ARRAY: len += STRNCPY( buf, size, "ARRAY" ); break;
				case NOT_CLASS: len += STRNCPY( buf, size, "CLASS" ); break;
			}
			break;
		}
#endif
		default:
		{
			len--;
		}
	}

	return len;
#endif
}

bool SQDebugServer::IsValidStackFrame( HSQUIRRELVM vm, int id )
{
	Assert( !!vm->_callsstacksize == !!vm->ci );
	return id >= 0 && id < vm->_callsstacksize && vm->_callsstacksize > 0;
}

int SQDebugServer::GetCurrentStackFrame( HSQUIRRELVM vm )
{
	if ( vm->ci && vm->_callsstacksize )
	{
		for ( int i = vm->_callsstacksize; i--; )
		{
			if ( vm->ci == &vm->_callsstack[i] )
			{
				return i;
			}
		}
	}

	return -1;
}

int SQDebugServer::GetStackBase( HSQUIRRELVM vm, int frameTarget )
{
	Assert( IsValidStackFrame( vm, frameTarget ) );

	int stackbase = 0;
	for ( int i = 0; i <= frameTarget; i++ )
		stackbase += vm->_callsstack[i]._prevstkbase;
	return stackbase;
}

void SQDebugServer::InitEnv_GetVal( SQObjectPtr &env )
{
	Assert( sq_type(env) == OT_NULL || is_delegable(env) && _delegable(env)->_delegate->_delegate );

	if ( is_delegable(env) )
		return;

	const char script[] =
		"{ [\"" KW_DELEGATE "\"] = null,"
		"_get = function(i) { return this[\"" KW_DELEGATE "\"][i]; },"
		// TODO: Send index to debugger to be able to set locals as well
		"_set = function(i, v) { return this[\"" KW_DELEGATE "\"][i] = v; },"
		"_newslot = function(i, v) { return this[\"" KW_DELEGATE "\"][i] <- v; }"
		"}";
	SQObjectPtr mt;
	RunScript( m_pRootVM, script, NULL, mt );
	Assert( sq_type(mt) == OT_TABLE );
	Assert( sq_type(env) == OT_NULL );
	env = SQTable::Create( _ss(m_pRootVM), 8 );
	_table(env)->SetDelegate( _table(mt) );
	_table(mt)->SetDelegate( _table(m_pRootVM->_roottable) );
}

void SQDebugServer::InitEnv_GetKey( SQObjectPtr &env )
{
	Assert( sq_type(env) == OT_NULL || is_delegable(env) && _delegable(env)->_delegate->_delegate );

	if ( is_delegable(env) )
		return;

	const char script[] =
		"{ [\"" KW_DELEGATE "\"] = null,"
		// Also checks _get
		"_get = function(i) { this[\"" KW_DELEGATE "\"][i]; return i; } }";
	SQObjectPtr mt;
	RunScript( m_pRootVM, script, NULL, mt );
	Assert( sq_type(mt) == OT_TABLE );
	Assert( sq_type(env) == OT_NULL );
	env = SQTable::Create( _ss(m_pRootVM), 0 );
	_table(env)->SetDelegate( _table(mt) );
	_table(mt)->SetDelegate( _table(m_pRootVM->_roottable) );
}

void SQDebugServer::SetEnvDelegate( SQObjectPtr &env, const SQObject &delegate )
{
	Assert( sq_type(env) != OT_NULL );
	Assert( _table(env)->_delegate );

	SQObjectPtr weakref;

	if ( ISREFCOUNTED( sq_type(delegate) ) )
		weakref = GetWeakRef( _refcounted(delegate), sq_type(delegate) );

	// Only deallocate if the thread is paused
	//
	// This will still block environment fallbacks,
	// so lookups that run while the thread is running (breakpoint conditions)
	// are expected to be explicit (`__this.var` instead of `var`)
	//
	// This is only an issue when there are environment variable names
	// that collide with local variable names.
	//
	// Ideally table nodes would be cleared without deallocation
	if ( m_State == ThreadState::Running )
	{
		SQObjectPtr key, val, _null;
		FOREACH_SQTABLE( _table(env), key, val )
			_table(env)->Set( key, _null );
	}
	else
	{
		_table(env)->Clear();
	}

	_table(env)->NewSlot( m_sqstrThis, weakref );
	_table(env)->_delegate->Set( m_sqstrDelegate, weakref );
}

void SQDebugServer::ClearEnvDelegate( SQObjectPtr &env )
{
	Assert( sq_type(env) != OT_NULL );
	Assert( _table(env)->_delegate );

	SQObjectPtr _null;
	_table(env)->Clear();
	_table(env)->NewSlot( m_sqstrThis, _null );
	_table(env)->_delegate->Set( m_sqstrDelegate, _null );
}

void SQDebugServer::SetEnvRoot( SQObjectPtr &env, const SQObject &root )
{
	Assert( sq_type(root) == OT_TABLE );
	Assert( sq_type(env) == OT_TABLE );
	Assert( _table(env)->_delegate );
	Assert( _table(env)->_delegate->_delegate );
	Assert( _table(env)->_delegate->_delegate != _table(root) );

	_table(env)->_delegate->_delegate = _table(root);
}

void SQDebugServer::CacheLocals( HSQUIRRELVM vm, int frame, SQObjectPtr &env )
{
	Assert( sq_type(env) != OT_NULL );
	Assert( IsValidStackFrame( vm, frame ) );

	const SQVM::CallInfo &ci = vm->_callsstack[ frame ];

	Assert( sq_type(ci._closure) == OT_CLOSURE );

	int stackbase = GetStackBase( vm, frame );
	SQClosure *pClosure = _closure(ci._closure);
	SQFunctionProto *func = _fp(pClosure->_function);

	SQUnsignedInteger ip = (SQUnsignedInteger)( ci._ip - func->_instructions ) - 1;

	SetEnvDelegate( env, vm->_stack._vals[ stackbase ] );

	for ( int i = func->_noutervalues; i--; )
	{
		const SQOuterVar &var = func->_outervalues[i];
		const SQObjectPtr &val = *_outervalptr( pClosure->_outervalues[i] );
		_table(env)->NewSlot( var._name, val );
	}

	for ( int i = func->_nlocalvarinfos; i--; )
	{
		const SQLocalVarInfo &var = func->_localvarinfos[i];
		if ( var._start_op <= ip && var._end_op >= ip )
		{
			const SQObjectPtr &val = vm->_stack._vals[ stackbase + var._pos ];
			_table(env)->NewSlot( var._name, val );
		}
	}
}

bool SQDebugServer::RunExpression( const string_t &expression, HSQUIRRELVM vm, int frame, SQObjectPtr &out,
		bool multiline )
{
	if ( !IsValidStackFrame( vm, frame ) )
		return RunScript( vm, expression, NULL, out, multiline );

	if ( expression.IsEqualTo( "this" ) )
	{
		out = vm->_stack._vals[ GetStackBase( vm, frame ) ];
		return true;
	}
	else if ( expression.IsEqualTo( INTERNAL_TAG("function") ) )
	{
		out = vm->_callsstack[ frame ]._closure;
		return true;
	}
	else if ( expression.IsEqualTo( INTERNAL_TAG("caller") ) )
	{
		if ( frame != 0 )
		{
			out = vm->_callsstack[ frame - 1 ]._closure;
			return true;
		}
		return false;
	}

	const SQVM::CallInfo &ci = vm->_callsstack[ frame ];

	if ( sq_type(ci._closure) != OT_CLOSURE )
		return false;

#ifdef CLOSURE_ROOT
	bool bRoot = _closure(ci._closure)->_root &&
		( _table(_closure(ci._closure)->_root->_obj) != _table(m_pRootVM->_roottable) );

	if ( bRoot )
		SetEnvRoot( m_EnvGetVal, _closure(ci._closure)->_root->_obj );
#endif

	CacheLocals( vm, frame, m_EnvGetVal );
	bool ret = RunScript( vm, expression, &m_EnvGetVal, out, multiline );

#ifdef CLOSURE_ROOT
	if ( bRoot )
		SetEnvRoot( m_EnvGetVal, m_pRootVM->_roottable );
#endif

	return ret;
}

bool SQDebugServer::CompileIdentifier( const string_t &expression, const SQObject &env, SQObjectPtr &out )
{
	SetEnvDelegate( m_EnvGetKey, ( sq_type(env) != OT_NULL ) ? env : m_pCurVM->_roottable );
	return RunScript( m_pCurVM, expression, &m_EnvGetKey, out );
}

bool SQDebugServer::CompileScript( const string_t &script, SQObjectPtr &out )
{
#ifdef SQUNICODE
	int size = sq_rsl( STRLEN("return()") + UnicodeLength( script.ptr ) + 1 );
#else
	int size = STRLEN("return()") + script.len + 1;
#endif
	SQChar *buf = (SQChar*)ScratchPad( size );

	memcpy( buf, _SC("return("), sq_rsl( STRLEN("return(") ) );
	buf += STRLEN("return(");

#ifdef SQUNICODE
	Assert( script.IsTerminated() );
	buf += UTF8ToUnicode( buf, script.ptr, size - ( (char*)buf - ScratchPad() ) );
#else
	memcpy( buf, script.ptr, script.len );
	buf += script.len;
#endif

	*buf++ = _SC(')');
	*buf = 0;

	Assert( ( (char*)buf - ScratchPad() ) == ( size - sq_rsl(1) ) );

	if ( SQ_SUCCEEDED( sq_compilebuffer( m_pCurVM, (SQChar*)ScratchPad(), size, _SC("sqdbg"), SQFalse ) ) )
	{
		// Don't create varargs on calls
		SQFunctionProto *fn = _fp(_closure(m_pCurVM->Top())->_function);
		if ( fn->_varparams )
		{
			fn->_varparams = false;
			fn->_nparameters--;
		}

		out = m_pCurVM->Top();
		m_pCurVM->Pop();
		return true;
	}

	return false;
}

bool SQDebugServer::RunScript( HSQUIRRELVM vm, const string_t &script, const SQObject *env, SQObjectPtr &out,
		bool multiline )
{
	int size;
	SQChar *buf;

	if ( !multiline )
	{
#ifdef SQUNICODE
		size = sq_rsl( STRLEN("return()") + UnicodeLength( script.ptr ) + 1 );
#else
		size = STRLEN("return()") + script.len + 1;
#endif
		buf = (SQChar*)ScratchPad( size );

		memcpy( buf, _SC("return("), sq_rsl( STRLEN("return(") ) );
		buf += STRLEN("return(");
	}
	else
	{
#ifdef SQUNICODE
		size = sq_rsl( UnicodeLength( script.ptr ) + 1 );
#else
		size = script.len + 1;
#endif
		buf = (SQChar*)ScratchPad( size );
	}

#ifdef SQUNICODE
	Assert( script.IsTerminated() );
	buf += UTF8ToUnicode( buf, script.ptr, size - ( (char*)buf - ScratchPad() ) );
#else
	memcpy( buf, script.ptr, script.len );
	buf += script.len;
#endif

	if ( !multiline )
		*buf++ = _SC(')');

	*buf = 0;

	Assert( ( (char*)buf - ScratchPad() ) == ( size - sq_rsl(1) ) );

	if ( SQ_SUCCEEDED( sq_compilebuffer( vm, (SQChar*)ScratchPad(), size, _SC("sqdbg"), SQFalse ) ) )
	{
		// Don't create varargs on calls
		SQFunctionProto *fn = _fp(_closure(vm->Top())->_function);
		if ( fn->_varparams )
		{
			fn->_varparams = false;
			fn->_nparameters--;
		}

		vm->Push( env ? *env : vm->_roottable );

		CCallGuard cg( this );

		// m_pCurVM will incorrectly change if a script is executed on a different thread.
		// save and restore
		HSQUIRRELVM curvm = m_pCurVM;

		if ( SQ_SUCCEEDED( sq_call( vm, 1, SQTrue, SQFalse ) ) )
		{
			m_pCurVM = curvm;
			out = vm->Top();
			vm->Pop();
			vm->Pop();
			return true;
		}

		m_pCurVM = curvm;
		vm->Pop();
	}

	return false;
}

bool SQDebugServer::RunClosure( const SQObjectPtr &closure, const SQObject *env, SQObjectPtr &ret )
{
	m_pCurVM->Push( closure );
	m_pCurVM->Push( env ? *env : m_pCurVM->_roottable );

	CCallGuard cg( this );

	if ( SQ_SUCCEEDED( sq_call( m_pCurVM, 1, SQTrue, SQFalse ) ) )
	{
		ret = m_pCurVM->Top();
		m_pCurVM->Pop();
		m_pCurVM->Pop();
		return true;
	}

	m_pCurVM->Pop();
	return false;
}

bool SQDebugServer::RunClosure( const SQObjectPtr &closure,
		const SQObject *env, const SQObjectPtr &p1, SQObjectPtr &ret )
{
	m_pCurVM->Push( closure );
	m_pCurVM->Push( env ? *env : m_pCurVM->_roottable );
	m_pCurVM->Push( p1 );

	CCallGuard cg( this );

	if ( SQ_SUCCEEDED( sq_call( m_pCurVM, 2, SQTrue, SQFalse ) ) )
	{
		ret = m_pCurVM->Top();
		m_pCurVM->Pop();
		m_pCurVM->Pop();
		return true;
	}

	m_pCurVM->Pop();
	return false;
}

bool SQDebugServer::RunClosure( const SQObjectPtr &closure,
		const SQObject *env, const SQObjectPtr &p1, const SQObjectPtr &p2, SQObjectPtr &ret )
{
	m_pCurVM->Push( closure );
	m_pCurVM->Push( env ? *env : m_pCurVM->_roottable );
	m_pCurVM->Push( p1 );
	m_pCurVM->Push( p2 );

	CCallGuard cg( this );

	if ( SQ_SUCCEEDED( sq_call( m_pCurVM, 3, SQTrue, SQFalse ) ) )
	{
		ret = m_pCurVM->Top();
		m_pCurVM->Pop();
		m_pCurVM->Pop();
		return true;
	}

	m_pCurVM->Pop();
	return false;
}

bool SQDebugServer::Get( const objref_t obj, SQObjectPtr &value )
{
	switch ( obj.type )
	{
		case objref_t::PTR:			value = *obj.ptr; return true;
		case objref_t::TABLE:		return obj.src.pTable->Get( obj.key, value );
		case objref_t::INSTANCE:	return obj.src.pInstance->Get( obj.key, value );
		case objref_t::CLASS:		return obj.src.pClass->Get( obj.key, value );
		case objref_t::INSTANCE_META:
		case objref_t::TABLE_META:
		case objref_t::USERDATA_META:
		{
			SQObjectPtr mm;
			if ( obj.src.pDelegable->GetMetaMethod( m_pRootVM, MT_GET, mm ) )
			{
				SQObject self;
				self._unVal = obj.src;

				switch ( obj.type )
				{
					case objref_t::INSTANCE_META: self._type = OT_INSTANCE; break;
					case objref_t::TABLE_META: self._type = OT_TABLE; break;
					case objref_t::USERDATA_META: self._type = OT_USERDATA; break;
					default: UNREACHABLE();
				}

				return RunClosure( mm, &self, obj.key, value );
			}

			return false;
		}
		// Exists only for data breakpoints to validate range
		// Otherwise arrays use objref_t::PTR
		case objref_t::ARRAY:
		{
			Assert( sq_type(obj.key) == OT_INTEGER );

			if ( _integer(obj.key) >= 0 && _integer(obj.key) < (SQInteger)obj.src.pArray->_values.size() )
			{
				value = obj.src.pArray->_values[ _integer(obj.key) ];
				return true;
			}

			return false;
		}
		default: UNREACHABLE();
	}
}

bool SQDebugServer::Set( const objref_t obj, const SQObjectPtr &value )
{
	switch ( obj.type )
	{
		case objref_t::PTR:			*(obj.ptr) = value; return true;
		case objref_t::TABLE:		return obj.src.pTable->Set( obj.key, value );
		case objref_t::INSTANCE:	return obj.src.pInstance->Set( obj.key, value );
		case objref_t::CLASS:
		{
			int tmp = GetSQClassLocked( obj.src.pClass );
			SetSQClassLocked( obj.src.pClass, 0 );
			bool res = obj.src.pClass->NewSlot( _ss(m_pRootVM), obj.key, value, false );
			SetSQClassLocked( obj.src.pClass, tmp );
			return res;
		}
		case objref_t::INSTANCE_META:
		case objref_t::TABLE_META:
		case objref_t::USERDATA_META:
		{
			SQObjectPtr mm;
			if ( obj.src.pDelegable->GetMetaMethod( m_pRootVM, MT_SET, mm ) )
			{
				SQObject self;
				self._unVal = obj.src;

				switch ( obj.type )
				{
					case objref_t::INSTANCE_META: self._type = OT_INSTANCE; break;
					case objref_t::TABLE_META: self._type = OT_TABLE; break;
					case objref_t::USERDATA_META: self._type = OT_USERDATA; break;
					default: UNREACHABLE();
				}

				SQObjectPtr dummy;
				return RunClosure( mm, &self, obj.key, value, dummy );
			}

			return false;
		}
		case objref_t::METAMETHOD_CLASS:
		{
			Assert( sq_type(value) == OT_CLOSURE || sq_type(value) == OT_NATIVECLOSURE );
			*(obj.ptr) = value;
			return true;
		}
		default: UNREACHABLE();
	}
}

bool SQDebugServer::GetObj( string_t &expression, bool identifierIsString, const SQObject &var, objref_t &out )
{
	if ( sq_type(var) == OT_ARRAY )
	{
		expression.ptr++;
		expression.len -= 2;

		SQInteger idx;
		if ( atoi( expression, &idx ) &&
				idx >= 0 && idx < (SQInteger)_array(var)->_values.size() )
		{
			out.type = objref_t::PTR;
			out.ptr = &_array(var)->_values[idx];

			out.src = var._unVal;
			out.key._type = OT_INTEGER;
			out.key._unVal.nInteger = idx;
			return true;
		}

		return false;
	}

	bool fallback = false;

	// Check for exact match first
	SQObjectPtr identifier;
	SQObjectPtr dummy;

	if ( identifierIsString )
	{
		identifier = CreateSQString( m_pRootVM, expression );
	}
	else
	{
		switch ( expression.ptr[0] )
		{
			// string
			case '\"':
			{
				expression.ptr++;
				expression.len -= 2;

				UndoEscape( expression.ptr, &expression.len );

			#ifdef SQUNICODE
				// UTF8ToUnicode expects terminated strings
				CRestoreVar<char> rz(
						expression.ptr+expression.len,
						expression.ptr[expression.len] );
				expression.ptr[expression.len] = 0;
			#endif

				identifier = CreateSQString( m_pRootVM, expression );
				break;
			}
			// integer
			case '[':
			{
				expression.ptr++;
				expression.len -= 2;

				SQInteger value;
				if ( !atoi( expression, &value ) )
					return false;

				identifier = value;
				break;
			}
			default:
			{
				// object, check every member
				if ( expression.StartsWith("0x") )
				{
					expression.len = FMT_PTR_LEN;

					SQUnsignedInteger pKey;
					if ( !atox( expression, &pKey ) )
						return false;

					SQObjectPtr obj = var;
					SQ_FOREACH_OP( obj, key, val )
					{
						if ( _rawval(key) == pKey )
						{
							switch ( sq_type(var) )
							{
								case OT_TABLE: out.type = objref_t::TABLE; break;
								case OT_INSTANCE: out.type = objref_t::INSTANCE; break;
								case OT_CLASS: out.type = objref_t::CLASS; break;
								default: UNREACHABLE();
							}
							out.src = var._unVal;
							out.key = key;
							return true;
						}
					}
					SQ_FOREACH_END()

					return false;
				}
				// float
				// NOTE: float keys are broken in pre 2.2.5 if sizeof(SQInteger) != sizeof(SQFloat),
				// this is fixed in SQ 2.2.5 with SQ_OBJECT_RAWINIT in SQObjectPtr operator=(SQFloat)
				else
				{
					identifier = (SQFloat)strtod( expression.ptr, NULL );
				}
			}
		}
	}

find:
	switch ( sq_type(var) )
	{
		case OT_TABLE:
		{
			if ( _table(var)->Get( identifier, dummy ) )
			{
				out.type = objref_t::TABLE;
				out.src = var._unVal;
				out.key = identifier;
				return true;
			}

			for ( SQTable *t = _delegable(var)->_delegate; t; t = t->_delegate )
			{
				if ( t->Get( identifier, dummy ) )
				{
					out.type = objref_t::TABLE;
					out.src.pTable = t;
					out.key = identifier;
					return true;
				}
			}

			break;
		}
		case OT_INSTANCE:
		{
			if ( _instance(var)->Get( identifier, dummy ) )
			{
				out.type = objref_t::INSTANCE;
				out.src = var._unVal;
				out.key = identifier;
				return true;
			}

			break;
		}
		case OT_CLASS:
		{
			if ( _class(var)->Get( identifier, dummy ) )
			{
				out.type = objref_t::CLASS;
				out.src = var._unVal;
				out.key = identifier;
				return true;
			}

			break;
		}
		default: break;
	}

	// metamethods
	switch ( sq_type(var) )
	{
		case OT_INSTANCE:
		case OT_TABLE:
		case OT_USERDATA:
		{
			SQObjectPtr mm;
			if ( _delegable(var)->GetMetaMethod( m_pRootVM, MT_GET, mm ) )
			{
				if ( RunClosure( mm, &var, identifier, dummy ) )
				{
					switch ( sq_type(var) )
					{
						case OT_INSTANCE: out.type = objref_t::INSTANCE_META; break;
						case OT_TABLE: out.type = objref_t::TABLE_META; break;
						case OT_USERDATA: out.type = objref_t::USERDATA_META; break;
						default: UNREACHABLE();
					}
					out.src = var._unVal;
					out.key = identifier;
					return true;
				}
			}

			break;
		}
		default: break;
	}

	if ( fallback )
		return false;

	// Expression wasn't an identifier, resolve it
	// This is slow!
	if ( !CompileIdentifier( expression, var, identifier ) )
		return false;

	fallback = true;
	goto find;
}

bool SQDebugServer::GetObj( const string_t &expression, HSQUIRRELVM vm, int frame, objref_t &out )
{
	// The expression can only be an identifier
	// Fast check without compiling
	if ( expression.Contains( "=(){}[]<>!\"':;,.+-/*|&^~%#\\ \t" ) ||
			( expression.ptr[0] >= '0' && expression.ptr[0] <= '9' ) )
		return false;

	if ( IsValidStackFrame( vm, frame ) )
	{
		const SQVM::CallInfo &ci = vm->_callsstack[ frame ];
		SQClosure *pClosure = _closure(ci._closure);
		SQFunctionProto *func = _fp(pClosure->_function);

		if ( sq_type(ci._closure) != OT_CLOSURE )
			return false;

		SQUnsignedInteger ip = (SQUnsignedInteger)( ci._ip - func->_instructions ) - 1;

		for ( int i = 0; i < func->_nlocalvarinfos; i++ )
		{
			const SQLocalVarInfo &var = func->_localvarinfos[i];
			if ( var._start_op <= ip && var._end_op >= ip &&
				expression.IsEqualTo( _string(var._name) ) )
			{
				int stackbase = GetStackBase( vm, frame );
				out.type = objref_t::PTR;
				out.ptr = &vm->_stack._vals[ stackbase + var._pos ];
				return true;
			}
		}

		for ( int i = 0; i < func->_noutervalues; i++ )
		{
			const SQOuterVar &var = func->_outervalues[i];
			if ( expression.IsEqualTo( _string(var._name) ) )
			{
				out.type = objref_t::PTR;
				out.ptr = _outervalptr( pClosure->_outervalues[i] );
				return true;
			}
		}
	}
	else
	{
		if ( expression.IsEqualTo( "this" ) )
		{
			out.type = objref_t::PTR;
			out.ptr = &vm->_roottable;
			return true;
		}
	}

	SQObjectPtr dummy;

#if SQUIRREL_VERSION_NUMBER >= 220
	SQTable *pConstTable = _table(_ss(vm)->_consts);
	if ( SQTable_Get( pConstTable, expression, dummy ) )
	{
		out.type = objref_t::TABLE;
		out.src.pTable = pConstTable;

		out.key._type = OT_STRING;
		out.key._unVal.pString = CreateSQString( m_pRootVM, expression );
		return true;
	}
#endif

	const SQObjectPtr &env = vm->_stack._vals[ vm->_stackbase ];
	if ( env._type == OT_TABLE )
	{
		if ( SQTable_Get( _table(env), expression, dummy ) )
		{
			out.type = objref_t::TABLE;
			out.src = env._unVal;

			out.key._type = OT_STRING;
			out.key._unVal.pString = CreateSQString( m_pRootVM, expression );
			return true;
		}
	}
	else if ( env._type == OT_INSTANCE )
	{
		SQString *pExpression = CreateSQString( m_pRootVM, expression );
		if ( _instance(env)->Get( pExpression, dummy ) )
		{
			out.type = objref_t::INSTANCE;
			out.src = env._unVal;

			out.key._type = OT_STRING;
			out.key._unVal.pString = CreateSQString( m_pRootVM, expression );
			return true;
		}
	}

	const SQObjectPtr &root = vm->_roottable;
	if ( SQTable_Get( _table(root), expression, dummy ) )
	{
		out.type = objref_t::TABLE;
		out.src = root._unVal;

		out.key._type = OT_STRING;
		out.key._unVal.pString = CreateSQString( m_pRootVM, expression );
		return true;
	}

	return false;
}

bool SQDebugServer::GetObj( string_t &expression, const varref_t *ref, objref_t &out )
{
	switch ( ref->type )
	{
		case VARREF_OBJ:
		{
			return GetObj( expression, !ref->obj.hasNonStringMembers, ref->GetVar(), out );
		}
		case VARREF_SCOPE_LOCALS:
		case VARREF_SCOPE_OUTERS:
		{
			return GetObj( expression, ref->GetThread(), ref->scope.stack, out );
		}
		case VARREF_OUTERS:
		{
			Assert( sq_type( ref->GetVar() ) == OT_CLOSURE );

			if ( sq_type( ref->GetVar() ) == OT_CLOSURE )
			{
				SQClosure *pClosure = _closure(ref->GetVar());
				SQFunctionProto *func = _fp(pClosure->_function);
				for ( int i = 0; i < func->_noutervalues; i++ )
				{
					const SQOuterVar &var = func->_outervalues[i];
					if ( expression.IsEqualTo( _string(var._name) ) )
					{
						out.type = objref_t::PTR;
						out.ptr = _outervalptr( pClosure->_outervalues[i] );
						return true;
					}
				}
			}

			return false;
		}
		case VARREF_LITERALS:
		{
			Assert( sq_type( ref->GetVar() ) == OT_CLOSURE );

			if ( sq_type( ref->GetVar() ) == OT_CLOSURE )
			{
				int idx;
				Verify( atoi( expression, &idx ) );
				out.type = objref_t::PTR;
				out.ptr = _fp(_closure(ref->GetVar())->_function)->_literals + idx;
				return true;
			}

			return false;
		}
		case VARREF_METAMETHODS:
		{
			int mm = -1;

			for ( int i = 0; i < MT_LAST; i++ )
			{
				if ( expression.IsEqualTo( g_MetaMethodName[i] ) )
				{
					mm = i;
					break;
				}
			}

			Assert( mm != -1 );

			if ( mm != -1 )
			{
				switch ( sq_type( ref->GetVar() ) )
				{
					case OT_CLASS:
					{
						out.type = objref_t::METAMETHOD_CLASS;
						out.ptr = &_class(ref->GetVar())->_metamethods[mm];
						return true;
					}
					case OT_TABLE:
					{
						// metamethods are regular members of tables
						Assert( _table(ref->GetVar())->_delegate );

						out.type = objref_t::TABLE;
						out.src.pTable = _table(ref->GetVar())->_delegate;

						out.key._type = OT_STRING;
						out.key._unVal.pString = CreateSQString( m_pRootVM, expression );
						return true;
					}
					default: Assert(0);
				}
			}

			return false;
		}
		case VARREF_INSTRUCTIONS:
		case VARREF_CALLSTACK:
			return false;

		default:
		{
			PrintError(_SC("(sqdbg) Invalid varref requested (%d)\n"), ref->type);
			AssertMsgF( 0, "Invalid varref requested (%d)", ref->type );
			return false;
		}
	}
}

static inline string_t GetPresentationHintKind( const SQObjectPtr &obj )
{
	switch ( sq_type(obj) )
	{
		case OT_CLOSURE:
		case OT_NATIVECLOSURE:
			return "method";
		case OT_CLASS:
			return "class";
		default:
			return "property";
	}
}

bool SQDebugServer::ShouldPageArray( const SQObject &obj, unsigned int limit )
{
	return ( sq_type(obj) == OT_ARRAY && _array(obj)->_values.size() > limit );
}

bool SQDebugServer::ShouldParseEvaluateName( const string_t &expression )
{
	return ( expression.len >= 4 && expression.ptr[0] == '@' && expression.ptr[2] == '@' );
}

bool SQDebugServer::ParseEvaluateName( const string_t &expression, HSQUIRRELVM vm, int frame, objref_t &out )
{
	Assert( ShouldParseEvaluateName( expression ) );

	if ( expression.ptr[1] == 'L' )
	{
		int idx;
		if ( !atoi( { expression.ptr + 3, expression.len - 3 }, &idx ) )
			return false;

		if ( !IsValidStackFrame( vm, frame ) )
			return false;

		const SQVM::CallInfo &ci = vm->_callsstack[ frame ];
		SQClosure *pClosure = _closure(ci._closure);
		SQFunctionProto *func = _fp(pClosure->_function);

		if ( sq_type(ci._closure) != OT_CLOSURE )
			return false;

		if ( idx >= func->_nlocalvarinfos )
			return false;

		SQUnsignedInteger ip = (SQUnsignedInteger)( ci._ip - func->_instructions ) - 1;

		const SQLocalVarInfo &var = func->_localvarinfos[idx];
		if ( var._start_op <= ip && var._end_op >= ip )
		{
			int stackbase = GetStackBase( vm, frame );
			out.type = objref_t::PTR;
			out.ptr = &vm->_stack._vals[ stackbase + var._pos ];
			return true;
		}
	}

	return false;
}

bool SQDebugServer::ParseBinaryNumber( const string_t &value, SQObject &out )
{
	// expect 0b prefix
	int inputbitlen = value.len - 2;

	if ( inputbitlen <= 0 )
		return false;

	if ( value.ptr[0] != '0' || value.ptr[1] != 'b' )
		return false;

	out._type = OT_INTEGER;
	out._unVal.nInteger = 0;

#ifdef _SQ64
	const int bitlen = 64;
#else
	const int bitlen = 32;
#endif
	if ( inputbitlen > bitlen )
		inputbitlen = bitlen;

	int target = value.len - inputbitlen;

	for ( int i = value.len - 1; i >= target; i-- )
	{
		if ( value.ptr[i] - '0' != 0 )
			out._unVal.nInteger |= ( (SQUnsignedInteger)1 << ( ( inputbitlen - 1 ) - ( i - 2 ) ) );
	}

	return true;
}

int SQDebugServer::ParseFormatSpecifiers( string_t &expression, char **ppComma )
{
	if ( expression.len <= 2 )
		return 0;

	int flags = 0;

	// 2 flags at most ",*x\0"
	char *start = expression.ptr + expression.len - 4;
	char *end = expression.ptr + expression.len;
	char *c = end;
	char *comma = NULL;

	if ( start < expression.ptr )
		start = expression.ptr + 1;

	for ( ; c > start; c-- )
	{
		if ( *c == ',' )
		{
			comma = c;
			c++;
			break;
		}
	}

	// have flags
	if ( comma )
	{
		// has to be the first flag
		if ( *c == '*' )
		{
			flags |= LOCK;
			c++;
		}

		if ( c < end )
		{
			switch ( *c )
			{
				case 'x': flags |= HEX; break;
				case 'b': flags |= BIN; break;
				case 'd': flags |= DEC; break;
				case 'f': flags |= FLT; break;
				default: return 0; // Invalid flag
			}
		}

		if ( flags )
		{
			expression.len = comma - expression.ptr;

			// Terminate here, this expression might be passed to SQTable::Get through GetObj,
			// which compares strings disregarding length
			*comma = 0;

			if ( ppComma )
				*ppComma = comma;
		}
	}

	return flags;
}

int SQDebugServer::EvalAndWriteExpr( char *buf, int size, string_t &expression, HSQUIRRELVM vm, int frame )
{
	// Don't modify log point string
	char *comma = NULL;

	// LOCK flag is ignored, it's ok
	int flags = NO_QUOT | ParseFormatSpecifiers( expression, &comma );

	CRestoreVar<char> rc( comma, ',' );

#ifdef SQUNICODE
	// UTF8ToUnicode expects terminated strings
	CRestoreVar<char> rz(
			expression.ptr+expression.len,
			expression.ptr[expression.len] );
	expression.ptr[expression.len] = 0;
#endif

	objref_t obj;

	// write to buffer in here because
	// res could be holding the only ref to a squirrel string
	// which the result string_t will point to
	SQObjectPtr res;

	if ( GetObj( expression, vm, frame, obj ) )
	{
		Get( obj, res );
		string_t result = GetValue( res, flags );

		int writelen = min( size, result.len );
		memcpy( buf, result.ptr, writelen );
		return writelen;
	}
	else if ( RunExpression( expression, vm, frame, res ) )
	{
		string_t result = GetValue( res, flags );

		int writelen = min( size, result.len );
		memcpy( buf, result.ptr, writelen );
		return writelen;
	}
	else
	{
		return 0;
	}
}

void SQDebugServer::OnRequest_Evaluate( const json_table_t &arguments, int seq )
{
	HSQUIRRELVM vm;
	int frame;

	string_t context, expression;

	arguments.GetString( "context", &context );
	arguments.GetString( "expression", &expression );
	arguments.GetInt( "frameId", &frame, -1 );

	TranslateFrameID( frame, &vm, &frame );

	int flags = 0;
	{
		json_table_t *format;
		if ( arguments.GetTable( "format", &format ) )
		{
			bool hex;
			format->GetBool( "hex", &hex );
			if ( hex )
				flags |= HEX;
		}
	}

	if ( expression.IsEmpty() )
	{
		DAP_ERROR_RESPONSE( seq, "evaluate" );
		DAP_ERROR_BODY( 0, "empty expression", 0 );
		DAP_SEND();
		return;
	}

	SQObjectPtr res;

	if ( ShouldParseEvaluateName( expression ) )
	{
		objref_t obj;

		if ( ParseEvaluateName( expression, vm, frame, obj ) )
		{
			Get( obj, res );

			DAP_START_RESPONSE( seq, "evaluate" );
			DAP_SET_TABLE( body, 5 );
				body.SetString( "result", GetValue( res, flags ) );
				body.SetString( "type", GetType( res ) );
				body.SetInt( "variablesReference", ToVarRef( res, true ) );
				json_table_t &hint = body.SetTable( "presentationHint", 1 );
				hint.SetString( "kind", GetPresentationHintKind( res ) );

				if ( ShouldPageArray( res, 1024 ) )
				{
					body.SetInt( "indexedVariables", (int)_array(res)->_values.size() );
				}
			DAP_SEND();

			return;
		}
	}

	if ( context.IsEqualTo( "repl" ) )
	{
		// Don't hit breakpoints unless it's repl
		m_bIgnoreDebugHookGuard = true;

		// Don't print quotes in repl
		flags |= NO_QUOT;
	}
	else if ( context.IsEqualTo( "clipboard" ) )
	{
		// Don't include address in class/instance values
		flags |= PURE;
	}

	if ( context.IsEqualTo( "watch" ) || context.IsEqualTo( "clipboard" ) )
	{
		flags |= ParseFormatSpecifiers( expression );

		if ( flags & LOCK )
		{
			int foundWatch = 0;

			for ( unsigned int i = 0; i < m_LockedWatches.size(); i++ )
			{
				watch_t &w = m_LockedWatches[i];
				if ( w.expression.IsEqualTo( expression ) )
				{
					vm = GetThread( w.thread );
					frame = w.frame;
					foundWatch = 1;
					break;
				}
			}

			if ( !foundWatch )
			{
				watch_t &w = m_LockedWatches.push_back();
				w.expression.Copy( expression );
				w.thread = GetWeakRef( vm );
				w.frame = frame;
			}
		}

		objref_t obj;

		if ( GetObj( expression, vm, frame, obj ) )
		{
			Get( obj, res );

			DAP_START_RESPONSE( seq, "evaluate" );
			DAP_SET_TABLE( body, 5 );
				body.SetString( "result", GetValue( res, flags ) );
				body.SetString( "type", GetType( res ) );
				body.SetInt( "variablesReference", ToVarRef( res, true ) );
				json_table_t &hint = body.SetTable( "presentationHint", 1 );
				hint.SetString( "kind", GetPresentationHintKind( res ) );

				if ( ShouldPageArray( res, 1024 ) )
				{
					body.SetInt( "indexedVariables", (int)_array(res)->_values.size() );
				}
			DAP_SEND();
		}
		else if ( RunExpression( expression, vm, frame, res ) )
		{
			DAP_START_RESPONSE( seq, "evaluate" );
			DAP_SET_TABLE( body, 5 );
				body.SetString( "result", GetValue( res, flags ) );
				body.SetString( "type", GetType( res ) );
				body.SetInt( "variablesReference", ToVarRef( res, true ) );
				json_table_t &hint = body.SetTable( "presentationHint", 2 );
				hint.SetString( "kind", GetPresentationHintKind( res ) );
				json_array_t &attributes = hint.SetArray( "attributes", 1 );
				attributes.Append( "readOnly" );

				if ( ShouldPageArray( res, 1024 ) )
				{
					body.SetInt( "indexedVariables", (int)_array(res)->_values.size() );
				}
			DAP_SEND();
		}
		else
		{
			if ( flags & LOCK )
			{
				for ( unsigned int i = 0; i < m_LockedWatches.size(); i++ )
				{
					watch_t &w = m_LockedWatches[i];
					if ( w.expression.IsEqualTo( expression ) )
					{
						w.expression.Free();
						m_LockedWatches.remove( i );
						break;
					}
				}
			}

			DAP_ERROR_RESPONSE( seq, "evaluate" );
			DAP_ERROR_BODY( 0, "could not evaluate expression", 0 );
			DAP_SEND();
		}
	}
	else
	{
		Assert( context.IsEqualTo( "repl" ) || context.IsEqualTo( "variables" ) || context.IsEqualTo( "hover" ) );

		if ( RunExpression( expression, vm, frame, res, expression.Contains('\n') ) )
		{
			DAP_START_RESPONSE( seq, "evaluate" );
			DAP_SET_TABLE( body, 3 );
				body.SetString( "result", GetValue( res, flags ) );
				body.SetInt( "variablesReference", ToVarRef( res, context.IsEqualTo( "repl" ) ) );

				if ( ShouldPageArray( res, 1024 ) )
				{
					body.SetInt( "indexedVariables", (int)_array(res)->_values.size() );
				}
			DAP_SEND();
		}
		else
		{
			DAP_ERROR_RESPONSE( seq, "evaluate" );
			DAP_ERROR_BODY( 0, "{reason}", 1 );
			json_table_t &variables = error.SetTable( "variables", 1 );
			variables.SetString( "reason", GetValue( vm->_lasterror, NO_QUOT ) );
			DAP_SEND();
		}
	}

	if ( context.IsEqualTo( "repl" ) )
		m_bIgnoreDebugHookGuard = false;
}

void SQDebugServer::OnRequest_Scopes( const json_table_t &arguments, int seq )
{
	HSQUIRRELVM vm;
	int frame;
	arguments.GetInt( "frameId", &frame, -1 );

	if ( !TranslateFrameID( frame, &vm, &frame ) )
	{
		DAP_ERROR_RESPONSE( seq, "scopes" );
		DAP_ERROR_BODY( 0, "invalid stack frame {id}", 1 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "id", GetValue( frame ) );
		DAP_SEND();
		return;
	}

	const SQVM::CallInfo &ci = vm->_callsstack[ frame ];
	if ( sq_type(ci._closure) != OT_CLOSURE )
	{
		DAP_ERROR_RESPONSE( seq, "scopes" );
		DAP_ERROR_BODY( 0, "native call frame", 0 );
		DAP_SEND();
		return;
	}

	SQClosure *pClosure = _closure(ci._closure);
	SQFunctionProto *func = _fp(pClosure->_function);

	DAP_START_RESPONSE( seq, "scopes" );
	DAP_SET_TABLE( body, 1 );
		json_array_t &scopes = body.SetArray( "scopes", 2 );
		{
			json_table_t &scope = scopes.AppendTable(4);
			scope.SetString( "name", "Locals" );
			scope.SetString( "presentationHint", "locals" );
			scope.SetBool( "expensive", false );
			scope.SetInt( "variablesReference", ToVarRef( VARREF_SCOPE_LOCALS, vm, frame ) );
		}
		if ( func->_noutervalues )
		{
			json_table_t &scope = scopes.AppendTable(4);
			scope.SetString( "name", "Outers" );
			scope.SetString( "presentationHint", "locals" );
			scope.SetBool( "expensive", false );
			scope.SetInt( "variablesReference", ToVarRef( VARREF_SCOPE_OUTERS, vm, frame ) );
		}
	DAP_SEND();
}

int SQDebugServer::ThreadToID( HSQUIRRELVM vm )
{
	for ( int i = m_Threads.size(); i--; )
	{
		SQWeakRef *wr = m_Threads[i];

		if ( !wr || sq_type( wr->_obj ) == OT_NULL )
		{
			m_Threads.remove(i);
			__ObjRelease( wr );
			continue;
		}

		Assert( sq_type( wr->_obj ) == OT_THREAD );

		if ( _thread(wr->_obj) == vm )
			return i;
	}

	SQWeakRef *wr = GetWeakRef( vm );
	__ObjAddRef( wr );

	int i = m_Threads.size();
	m_Threads.push_back( wr );
	return i;
}

HSQUIRRELVM SQDebugServer::ThreadFromID( int id )
{
	if ( id >= 0 && id < (int)m_Threads.size() )
	{
		SQWeakRef *wr = m_Threads[id];

		if ( !wr || sq_type( wr->_obj ) == OT_NULL )
		{
			m_Threads.remove(id);
			__ObjRelease( wr );
			return NULL;
		}

		Assert( sq_type( wr->_obj ) == OT_THREAD );

		return _thread(wr->_obj);
	}

	return NULL;
}

void SQDebugServer::RemoveThreads()
{
	for ( int i = m_Threads.size(); i--; )
	{
		SQWeakRef *wr = m_Threads[i];
		Assert( wr );
		__ObjRelease( wr );
	}

	m_Threads.resize(0);
}

void SQDebugServer::OnRequest_Threads( int seq )
{
	DAP_START_RESPONSE( seq, "threads" );
	DAP_SET_TABLE( body, 1 );
		json_array_t &threads = body.SetArray( "threads", 1 );

		for ( unsigned int i = 0; i < m_Threads.size(); i++ )
		{
			SQWeakRef *wr = m_Threads[i];

			if ( !wr || sq_type( wr->_obj ) == OT_NULL )
			{
				m_Threads.remove(i);
				__ObjRelease( wr );
				i--;
				continue;
			}

			json_table_t &thread = threads.AppendTable(2);
			thread.SetInt( "id", i );

			Assert( sq_type( wr->_obj ) == OT_THREAD );

			if ( _thread( wr->_obj ) == m_pRootVM )
			{
				thread.SetString( "name", "MainThread" );
			}
			else
			{
				stringbuf_t< STRLEN("Thread ") + FMT_PTR_LEN + 1 > name;
				name.Puts( "Thread " );
				name.PutHex( (SQUnsignedInteger)_thread( wr->_obj ) );
				thread.SetString( "name", name );
			}
		}
	DAP_SEND();
}

bool SQDebugServer::ShouldIgnoreStackFrame( const SQVM::CallInfo &ci )
{
	// Ignore SQDebugServer::RunScript() (first frame)
	if ( sq_type(ci._closure) == OT_CLOSURE &&
			sq_type(_fp(_closure(ci._closure)->_function)->_sourcename) == OT_STRING &&
			sqstring_t(_SC("sqdbg")).IsEqualTo( _string(_fp(_closure(ci._closure)->_function)->_sourcename) ) )
		return true;

	// Ignore error handler / debug hook (last frame)
	if ( sq_type(ci._closure) == OT_NATIVECLOSURE && (
				_nativeclosure(ci._closure)->_function == &SQDebugServer::SQErrorHandler
#ifndef NATIVE_DEBUG_HOOK
			|| _nativeclosure(ci._closure)->_function == &SQDebugServer::SQDebugHook
#endif
			) )
		return true;

	return false;
}

int SQDebugServer::ConvertToFrameID( int threadId, int stackFrame )
{
	for ( unsigned int i = 0; i < m_FrameIDs.size(); i++ )
	{
		frameid_t &v = m_FrameIDs[i];
		if ( v.threadId == threadId && v.frame == stackFrame )
			return i;
	}

	int i = m_FrameIDs.size();

	frameid_t &v = m_FrameIDs.push_back();
	v.threadId = threadId;
	v.frame = stackFrame;

	return i;
}

bool SQDebugServer::TranslateFrameID( int frameId, HSQUIRRELVM *thread, int *stackFrame )
{
	if ( frameId >= 0 && frameId < (int)m_FrameIDs.size() )
	{
		frameid_t &v = m_FrameIDs[frameId];
		*thread = ThreadFromID( v.threadId );
		*stackFrame = v.frame;

		return IsValidStackFrame( *thread, *stackFrame );
	}

	*thread = m_pCurVM;
	return false;
}

void SQDebugServer::OnRequest_StackTrace( const json_table_t &arguments, int seq )
{
	int threadId, startFrame, levels;

	arguments.GetInt( "threadId", &threadId );
	arguments.GetInt( "startFrame", &startFrame );
	arguments.GetInt( "levels", &levels );

	HSQUIRRELVM vm = ThreadFromID( threadId );
	if ( !vm )
	{
		DAP_ERROR_RESPONSE( seq, "stackTrace" );
		DAP_ERROR_BODY( 0, "invalid thread {id}", 1 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "id", GetValue( threadId ) );
		DAP_SEND();
		return;
	}

	int lastFrame = vm->_callsstacksize - 1;

	if ( startFrame > lastFrame )
	{
		DAP_ERROR_RESPONSE( seq, "stackTrace" );
		DAP_ERROR_BODY( 0, "", 0 );
		DAP_SEND();
		return;
	}

	if ( startFrame < 0 )
		startFrame = 0;

	// reverse
	startFrame = lastFrame - startFrame;

	if ( levels <= 0 || levels > lastFrame )
		levels = lastFrame;

	int targetFrame = startFrame - levels;

	if ( targetFrame < 0 )
		targetFrame = 0;

	_DAP_START_RESPONSE( seq, "stackTrace", true, 2 );
	DAP_SET_TABLE( body, 1 );
		json_array_t &stackFrames = body.SetArray( "stackFrames", startFrame - targetFrame + 1 );
		for ( int i = startFrame; i >= targetFrame; i-- )
		{
			const SQVM::CallInfo &ci = vm->_callsstack[i];

			if ( ShouldIgnoreStackFrame(ci) )
				continue;

			if ( sq_type(ci._closure) == OT_CLOSURE )
			{
				json_table_t &frame = stackFrames.AppendTable(6);
				frame.SetInt( "id", ConvertToFrameID( threadId, i ) );

				SQFunctionProto *func = _fp(_closure(ci._closure)->_function);

				stringbuf_t< 256 > name;

				if ( sq_type(func->_name) == OT_STRING )
				{
					name.Puts( _string(func->_name) );
				}
				else
				{
					name.PutHex( (SQUnsignedInteger)func );
				}

				name.Put('(');

				Assert( func->_nparameters );

				int nparams = func->_nparameters;
				if ( nparams > 1 )
				{
					if ( func->_varparams )
						nparams--;

					for ( int j = 1; j < nparams; j++ )
					{
						const SQObjectPtr &param = func->_parameters[j];
						if ( sq_type(param) == OT_STRING )
						{
							name.Puts( _string(param) );
							name.Put(',');
							name.Put(' ');
						}
					}

					if ( !func->_varparams )
					{
						name.len -= 2;
					}
					else
					{
						name.Put('.');
						name.Put('.');
						name.Put('.');
					}
				}

				name.Put(')');
				name.Term();

				frame.SetString( "name", name );

				if ( sq_type(func->_sourcename) == OT_STRING )
				{
					json_table_t &source = frame.SetTable( "source", 2 );
					source.SetString( "name", sqstring_t( _string(func->_sourcename) ) );
					sqstring_t *path = m_FilePathMap.Get( _string(func->_sourcename) );
					if ( path )
						source.SetString( "path", *path );
				}

				frame.SetInt( "line", (int)func->GetLine( ci._ip ) );
				frame.SetInt( "column", 1 );

				stringbuf_t< FMT_PTR_LEN + 1 > instrref;
				instrref.PutHex( (SQUnsignedInteger)ci._ip );
				frame.SetString( "instructionPointerReference", instrref );
			}
			else if ( sq_type(ci._closure) == OT_NATIVECLOSURE )
			{
				json_table_t &frame = stackFrames.AppendTable(6);
				frame.SetInt( "id", ConvertToFrameID( threadId, i ) );

				SQNativeClosure *closure = _nativeclosure(ci._closure);

				json_table_t &source = frame.SetTable( "source", 1 );
				source.SetString( "name", "NATIVE" );

				stringbuf_t< 128 > name;

				if ( sq_type(closure->_name) == OT_STRING )
				{
					name.Puts( _string(closure->_name) );
				}
				else
				{
					name.PutHex( (SQUnsignedInteger)closure );
				}

				name.Put('(');
				name.Put(')');
				name.Term();

				frame.SetString( "name", name );

				frame.SetInt( "line", -1 );
				frame.SetInt( "column", 1 );
				frame.SetString( "presentationHint", "subtle" );
			}
			else UNREACHABLE();
		}
	DAP_SET( "totalFrames", lastFrame + 1 );
	DAP_SEND();
}

static bool HasMetaMethods( HSQUIRRELVM vm, const SQObjectPtr &obj )
{
	switch ( sq_type(obj) )
	{
		case OT_CLASS:
		{
			for ( unsigned int i = 0; i < MT_LAST; i++ )
			{
				if ( sq_type( _class(obj)->_metamethods[i] ) != OT_NULL )
				{
					return true;
				}
			}

			return false;
		}
		default:
		{
			if ( is_delegable(obj) && _delegable(obj)->_delegate )
			{
				SQObjectPtr dummy;

				for ( unsigned int i = 0; i < MT_LAST; i++ )
				{
					if ( _delegable(obj)->GetMetaMethod( vm, (SQMetaMethod)i, dummy ) )
					{
						return true;
					}
				}
			}

			return false;
		}
	}
}

static bool DelegateContainsObject( const SQObjectPtr &target, const SQObjectPtr &key, const SQObjectPtr &val )
{
	switch ( sq_type(target) )
	{
		case OT_CLASS:
		{
			if ( _class(target)->_base )
			{
				SQObjectPtr res;

				if ( _class(target)->_base->Get( key, res ) )
				{
					return ( _rawval(res) == _rawval(val) );
				}
			}

			return false;
		}
		case OT_INSTANCE:
		{
			Assert( _instance(target)->_class );
			SQObjectPtr res;

			if ( _instance(target)->_class->Get( key, res ) )
			{
				return ( _rawval(res) == _rawval(val) );
			}

			return false;
		}
		case OT_TABLE:
		{
			if ( _table(target)->_delegate )
			{
				SQObjectPtr res;

				if ( _table(target)->_delegate->Get( key, res ) )
				{
					return ( _rawval(res) == _rawval(val) );
				}
			}

			return false;
		}
		default: return false;
	}
}

static bool HasNonStringMembers( const SQObject &o )
{
	SQObjectPtr obj = o;

	switch ( sq_type(obj) )
	{
		case OT_TABLE:
		{
			SQObjectPtr key, val;
			FOREACH_SQTABLE( _table(obj), key, val )
			{
				if ( !sq_isstring(key) )
					return true;
			}
			return false;
		}
		case OT_INSTANCE:
		{
			SQObjectPtr key, val;
			SQClass *base = _instance(obj)->_class;
			FOREACH_SQCLASS( base, key, val )
			{
				if ( !sq_isstring(key) )
					return true;
			}
			return false;
		}
		case OT_CLASS:
		{
			SQObjectPtr key, val;
			FOREACH_SQCLASS( _class(obj), key, val )
			{
				if ( !sq_isstring(key) )
					return true;
			}
			return false;
		}
		default:
			return false;
	}
}

static inline void SetVirtualHint( json_table_t &elem )
{
	json_table_t &hint = elem.SetTable( "presentationHint", 2 );
	hint.SetString( "kind", "virtual" );
	json_array_t &attributes = hint.SetArray( "attributes", 1 );
	attributes.Append( "readOnly" );
}

static int _sortkeys( const void * const a, const void * const b )
{
	const SQObjectPtr &oa = *(const SQObjectPtr *)a;
	const SQObjectPtr &ob = *(const SQObjectPtr *)b;

	if ( sq_isstring(oa) )
	{
		if ( sq_isstring(ob) )
		{
			return scstricmp( _stringval(oa), _stringval(ob) );
		}
		else
		{
			return 1;
		}
	}
	else
	{
		if ( sq_isstring(ob) )
		{
			return -1;
		}
		else
		{
			return _rawval(oa) - _rawval(ob);
		}
	}
}

// Sort table members
// If there exists non-string members, quote all string members to distinguish them from others
// E.g. { [0] = 1, ["0"] = 2, ["\"0\""] = 3 }
static void SortKeys( const SQObjectPtr &obj, bool *pShouldQuoteKeys, sqvector< SQObjectPtr > *array )
{
	bool shouldQuoteKeys = false;

#define _check(key) \
		array->push_back( key ); \
		if ( !sq_isstring( key ) ) \
			shouldQuoteKeys = true;

	switch ( sq_type(obj) )
	{
		case OT_TABLE:
		{
			SQObjectPtr key, val;
			FOREACH_SQTABLE( _table(obj), key, val )
			{
				_check(key);
			}
			break;
		}
		case OT_INSTANCE:
		{
			SQObjectPtr key, val;
			SQClass *base = _instance(obj)->_class;
			FOREACH_SQCLASS( base, key, val )
			{
				_check(key);
			}
			break;
		}
		case OT_CLASS:
		{
			SQObjectPtr key, val;
			FOREACH_SQCLASS( _class(obj), key, val )
			{
				_check(key);
			}
			break;
		}
		default: UNREACHABLE();
	}
#undef _check

	*pShouldQuoteKeys = shouldQuoteKeys;

	qsort( array->_vals, array->size(), sizeof(SQObjectPtr), _sortkeys );
}

void SQDebugServer::OnRequest_Variables( const json_table_t &arguments, int seq )
{
	int variablesReference;
	arguments.GetInt( "variablesReference", &variablesReference );

	varref_t *ref = FromVarRef( variablesReference );
	if ( !ref )
	{
		DAP_ERROR_RESPONSE( seq, "variables" );
		DAP_ERROR_BODY( 0, "failed to find variable", 0 );
		DAP_SEND();
		return;
	}

	int flags = 0;
	{
		json_table_t *format;
		if ( arguments.GetTable( "format", &format ) )
		{
			bool hex;
			format->GetBool( "hex", &hex );
			if ( hex )
				flags |= HEX;
		}
	}

	switch ( ref->type )
	{
		case VARREF_SCOPE_LOCALS:
		{
			HSQUIRRELVM vm = ref->GetThread();
			int frame = ref->scope.stack;

			if ( !IsValidStackFrame( vm, frame ) ||
					sq_type(vm->_callsstack[frame]._closure) != OT_CLOSURE )
			{
				DAP_ERROR_RESPONSE( seq, "variables" );
				DAP_ERROR_BODY( 0, "invalid stack frame {id}", 1 );
				json_table_t &variables = error.SetTable( "variables", 1 );
				variables.SetString( "id", GetValue( frame ) );
				DAP_SEND();
				break;
			}

			DAP_START_RESPONSE( seq, "variables" );
			DAP_SET_TABLE( body, 1 );
				json_array_t &variables = body.SetArray( "variables", 8 );

				if ( sq_type(m_ReturnValue) != OT_NULL )
				{
					json_table_t &elem = variables.AppendTable(5);
					elem.SetString( "name", "@return@" );
					elem.SetString( "value", GetValue( m_ReturnValue, flags ) );
					elem.SetString( "type", GetType( m_ReturnValue ) );
					elem.SetInt( "variablesReference", ToVarRef( m_ReturnValue ) );
					SetVirtualHint( elem );
				}

				const SQVM::CallInfo &ci = vm->_callsstack[ frame ];
				int stackbase = GetStackBase( vm, frame );
				SQClosure *pClosure = _closure(ci._closure);
				SQFunctionProto *func = _fp(pClosure->_function);

				SQUnsignedInteger ip = (SQUnsignedInteger)( ci._ip - func->_instructions ) - 1;

				for ( int i = func->_nlocalvarinfos; i--; )
				{
					const SQLocalVarInfo &var = func->_localvarinfos[i];
					if ( var._start_op <= ip && var._end_op >= ip )
					{
						const SQObjectPtr &val = vm->_stack._vals[ stackbase + var._pos ];
						json_table_t &elem = variables.AppendTable(5);
						elem.SetString( "name", sqstring_t( _string(var._name) ) );
						elem.SetString( "value", GetValue( val, flags ) );
						elem.SetString( "type", GetType( val ) );
						stringbuf_t< 8 > buf;
						buf.Put('@'); buf.Put('L'); buf.Put('@'); buf.PutInt(i);
						elem.SetString( "evaluateName", buf );
						elem.SetInt( "variablesReference", ToVarRef( val ) );
					}
				}
			DAP_SEND();

			break;
		}
		case VARREF_SCOPE_OUTERS:
		{
			HSQUIRRELVM vm = ref->GetThread();
			int frame = ref->scope.stack;

			if ( !IsValidStackFrame( vm, frame ) ||
					sq_type(vm->_callsstack[frame]._closure) != OT_CLOSURE )
			{
				DAP_ERROR_RESPONSE( seq, "variables" );
				DAP_ERROR_BODY( 0, "invalid stack frame {id}", 1 );
				json_table_t &variables = error.SetTable( "variables", 1 );
				variables.SetString( "id", GetValue( frame ) );
				DAP_SEND();
				break;
			}

			const SQVM::CallInfo &ci = vm->_callsstack[ frame ];
			SQClosure *pClosure = _closure(ci._closure);
			SQFunctionProto *func = _fp(pClosure->_function);

			DAP_START_RESPONSE( seq, "variables" );
			DAP_SET_TABLE( body, 1 );
				json_array_t &variables = body.SetArray( "variables", func->_noutervalues );
				for ( int i = func->_noutervalues; i--; )
				{
					const SQOuterVar &var = func->_outervalues[i];
					Assert( sq_type(var._name) == OT_STRING );

					const SQObjectPtr &val = *_outervalptr( pClosure->_outervalues[i] );
					json_table_t &elem = variables.AppendTable(4);
					elem.SetString( "name", sqstring_t( _string(var._name) ) );
					elem.SetString( "value", GetValue( val, flags ) );
					elem.SetString( "type", GetType( val ) );
					elem.SetInt( "variablesReference", ToVarRef( val ) );
				}
			DAP_SEND();

			break;
		}
		case VARREF_OBJ:
		{
			SQObject target = ref->GetVar();

			if ( !ISREFCOUNTED( sq_type(target) ) )
			{
				DAP_ERROR_RESPONSE( seq, "variables" );
				DAP_ERROR_BODY( 0, "invalid object", 0 );
				DAP_SEND();
				return;
			}

			DAP_START_RESPONSE( seq, "variables" );
			DAP_SET_TABLE( body, 1 );
				json_array_t &variables = body.SetArray( "variables", 4 );

				{
					json_table_t &elem = variables.AppendTable(4);
					elem.SetString( "name", INTERNAL_TAG("refs") );
					elem.SetInt( "variablesReference", -1 );
					SetVirtualHint( elem );

					if ( sq_type(target) != OT_WEAKREF )
					{
						elem.SetString( "value", GetValue( _refcounted(target)->_uiRef, flags ) );
					}
					else
					{
						stringbuf_t< 256 > buf;
						buf.PutInt( _refcounted(target)->_uiRef );

						do
						{
							target = _weakref(target)->_obj;
							buf.Put(' ');
							buf.Put('>');
							buf.Put( ( sq_type(target) != OT_WEAKREF ) ? '*' : ' ' );
							buf.PutInt( _refcounted(target)->_uiRef );

							if ( buf.len >= sizeof(buf.ptr) - 4 )
								break;
						}
						while ( sq_type(target) == OT_WEAKREF );

						buf.Term();

						elem.SetString( "value", buf );
					}
				}

				if ( sq_type(target) == OT_ARRAY )
				{
					SQObjectPtrVec &vals = _array(target)->_values;

					json_table_t &elem = variables.AppendTable(4);
					elem.SetString( "name", INTERNAL_TAG("allocated") );
					elem.SetString( "value", GetValue( vals.capacity(), flags ) );
					elem.SetInt( "variablesReference", -1 );
					SetVirtualHint( elem );

					int idx, end;

					string_t filter;
					if ( arguments.GetString( "filter", &filter ) && filter.IsEqualTo("indexed") )
					{
						arguments.GetInt( "start", &idx );
						arguments.GetInt( "count", &end );

						if ( idx < 0 )
							idx = 0;

						if ( end <= 0 )
						{
							end = (int)vals.size();
						}
						else
						{
							end += idx;
							if ( end > (int)vals.size() )
								end = (int)vals.size();
						}
					}
					else
					{
						idx = 0;
						end = (int)vals.size();
					}

					for ( ; idx < end; idx++ )
					{
						const SQObjectPtr &val = vals[idx];

						stringbuf_t< 32 > name;
						name.Put('[');
						name.PutInt( idx );
						name.Put(']');
						name.Term();

						json_table_t &elem = variables.AppendTable(5);
						elem.SetString( "name", name );
						elem.SetString( "value", GetValue( val, flags ) );
						elem.SetString( "type", GetType( val ) );
						elem.SetInt( "variablesReference", ToVarRef( val ) );
						json_table_t &hint = elem.SetTable( "presentationHint", 1 );
						hint.SetString( "kind", GetPresentationHintKind( val ) );
					}

					// done with arrays
				}

				// delegates
				switch ( sq_type(target) )
				{
					case OT_INSTANCE:
					{
						if ( _instance(target)->_class )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("class") );
							elem.SetString( "value", GetValue( ToSQObject( _instance(target)->_class ) ) );
							elem.SetInt( "variablesReference", ToVarRef( ToSQObject( _instance(target)->_class ) ) );
							SetVirtualHint( elem );
						}
						break;
					}
					case OT_CLASS:
					{
						if ( _class(target)->_base )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("base") );
							elem.SetString( "value", GetValue( ToSQObject( _class(target)->_base ) ) );
							elem.SetInt( "variablesReference", ToVarRef( ToSQObject( _class(target)->_base ) ) );
							SetVirtualHint( elem );
						}
						break;
					}
					case OT_TABLE:
					{
						if ( _table(target)->_delegate )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("delegate") );
							elem.SetString( "value", GetValue( ToSQObject( _table(target)->_delegate ) ) );
							elem.SetInt( "variablesReference", ToVarRef( ToSQObject( _table(target)->_delegate ) ) );
							SetVirtualHint( elem );
						}
						break;
					}
					default: break;
				}

				// metamembers
				if ( sq_type(target) == OT_INSTANCE )
				{
					SQClass *base = _instance(target)->_class;

					for ( unsigned int i = 0; i < m_ClassDefinitions.size(); i++ )
					{
						const classdef_t &def = m_ClassDefinitions[i];

						if ( def.base != base )
							continue;

						if ( sq_type( def.metamembers ) == OT_NULL )
							break;

						SQObjectPtr mm;
						if ( !_instance(target)->GetMetaMethod( m_pRootVM, MT_GET, mm ) )
							break;

						for ( unsigned int i = 0; i < _array(def.metamembers)->_values.size(); i++ )
						{
							SQObjectPtr val;
							SQObjectPtr &member = _array(def.metamembers)->_values[i];

							if ( RunClosure( mm, &target, member, val ) )
							{
								json_table_t &elem = variables.AppendTable(5);
								elem.SetString( "name", GetValue( member, NO_QUOT ) );
								elem.SetString( "value", GetValue( val, flags ) );
								elem.SetString( "type", GetType( val ) );
								elem.SetInt( "variablesReference", ToVarRef( val ) );
								json_table_t &hint = elem.SetTable( "presentationHint", 1 );
								hint.SetString( "kind", GetPresentationHintKind( val ) );
							}
						}
					}
				}
				// metamethods
				else if ( HasMetaMethods( m_pRootVM, target ) )
				{
					json_table_t &elem = variables.AppendTable(4);
					elem.SetString( "name", INTERNAL_TAG("metamethods") );
					elem.SetString( "value", "{...}" );
					elem.SetInt( "variablesReference", ToVarRef( VARREF_METAMETHODS, target ) );
					SetVirtualHint( elem );
				}

				// members
				switch ( sq_type(target) )
				{
					case OT_TABLE:
					case OT_CLASS:
					case OT_USERDATA:
					{
						SQObjectPtr obj = target;

						bool shouldQuoteKeys;
						sqvector< SQObjectPtr > keys;

						switch ( sq_type(target) )
						{
							case OT_TABLE: keys.reserve( _table(target)->CountUsed() ); break;
							case OT_CLASS: keys.reserve( _class(target)->_members->CountUsed() ); break;
							default: break;
						}

						SortKeys( obj, &shouldQuoteKeys, &keys );

						if ( !shouldQuoteKeys )
							flags |= NO_QUOT;

						for ( unsigned int i = 0; i < keys.size(); i++ )
						{
							const SQObjectPtr &key = keys[i];
							SQObjectPtr val;

							m_pCurVM->Get( obj, key, val, 0, 0 );

							switch ( sq_type(val) )
							{
								case OT_CLOSURE:
								case OT_NATIVECLOSURE:
								case OT_CLASS:
								{
									// ignore delegate functions
									if ( DelegateContainsObject( target, key, val ) )
										continue;
								}
								default: break;
							}

							json_table_t &elem = variables.AppendTable(5);

							if ( shouldQuoteKeys && sq_type(key) == OT_INTEGER )
							{
								stringbuf_t< 32 > name;
								name.Put('[');
								name.PutInt( _integer(key) );
								name.Put(']');
								name.Term();
								elem.SetString( "name", name );
							}
							else
							{
								elem.SetString( "name", GetValue( key, flags ) );
							}

							elem.SetString( "value", GetValue( val, flags ) );
							elem.SetString( "type", GetType( val ) );
							elem.SetInt( "variablesReference", ToVarRef( val ) );
							json_table_t &hint = elem.SetTable( "presentationHint", 1 );
							hint.SetString( "kind", GetPresentationHintKind( val ) );
						}

						break;
					}
					case OT_INSTANCE:
					{
						SQObjectPtr base = _instance(target)->_class;

						bool shouldQuoteKeys;
						sqvector< SQObjectPtr > keys;
						keys.reserve( _instance(target)->_class->_members->CountUsed() );

						SortKeys( base, &shouldQuoteKeys, &keys );

						if ( !shouldQuoteKeys )
							flags |= NO_QUOT;

						for ( unsigned int i = 0; i < keys.size(); i++ )
						{
							const SQObjectPtr &key = keys[i];
							SQObjectPtr val;

							_instance(target)->Get( key, val );

							switch ( sq_type(val) )
							{
								case OT_CLOSURE:
								case OT_NATIVECLOSURE:
								case OT_CLASS:
								{
									// ignore delegate functions
									if ( DelegateContainsObject( target, key, val ) )
										continue;
								}
								default: break;
							}

							json_table_t &elem = variables.AppendTable(5);

							if ( shouldQuoteKeys && sq_type(key) == OT_INTEGER )
							{
								stringbuf_t< 32 > name;
								name.Put('[');
								name.PutInt( _integer(key) );
								name.Put(']');
								name.Term();
								elem.SetString( "name", name );
							}
							else
							{
								elem.SetString( "name", GetValue( key, flags ) );
							}

							elem.SetString( "value", GetValue( val, flags ) );
							elem.SetString( "type", GetType( val ) );
							elem.SetInt( "variablesReference", ToVarRef( val ) );
							json_table_t &hint = elem.SetTable( "presentationHint", 1 );
							hint.SetString( "kind", GetPresentationHintKind( val ) );
						}

						break;
					}
					case OT_CLOSURE:
					{
						SQFunctionProto *func = _fp(_closure(target)->_function);

						if ( sq_type( func->_name ) != OT_NULL )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("name") );
							elem.SetString( "value", GetValue( func->_name ) );
							elem.SetInt( "variablesReference", -1 );
							SetVirtualHint( elem );
						}

						if ( sq_type( func->_sourcename ) != OT_NULL )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("source") );
							elem.SetString( "value", GetValue( func->_sourcename ) );
							elem.SetInt( "variablesReference", -1 );
							SetVirtualHint( elem );
						}

						if ( func->_bgenerator )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("generator") );
							elem.SetString( "value", "1" );
							elem.SetInt( "variablesReference", -1 );
							SetVirtualHint( elem );
						}

						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("stacksize") );
							elem.SetString( "value", GetValue( func->_stacksize, flags ) );
							elem.SetInt( "variablesReference", -1 );
							SetVirtualHint( elem );
						}

						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("instructions") );

							// ignore line ops
							SQInstruction *instr = func->_instructions;
							int c = func->_ninstructions;
							for ( int i = c; i--; )
								if ( instr[i].op == _OP_LINE )
									c--;

							elem.SetString( "value", GetValue( c, flags ) );
							elem.SetInt( "variablesReference", ToVarRef( VARREF_INSTRUCTIONS, target ) );
							SetVirtualHint( elem );
						}

						if ( func->_nliterals )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("literals") );
							elem.SetString( "value", GetValue( func->_nliterals, flags ) );
							elem.SetInt( "variablesReference", ToVarRef( VARREF_LITERALS, target ) );
							SetVirtualHint( elem );
						}

						if ( func->_noutervalues )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("outervalues") );
							elem.SetString( "value", GetValue( func->_noutervalues, flags ) );
							elem.SetInt( "variablesReference", ToVarRef( VARREF_OUTERS, target ) );
							SetVirtualHint( elem );
						}
#ifdef CLOSURE_ENV
						if ( CLOSURE_ENV_ISVALID( _closure(target)->_env ) )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("env") );
							elem.SetString( "value", GetValue(
										CLOSURE_ENV_OBJ( _closure(target)->_env ) ) );
							elem.SetInt( "variablesReference", ToVarRef(
										CLOSURE_ENV_OBJ( _closure(target)->_env ) ) );
							SetVirtualHint( elem );
						}
#endif
#ifdef CLOSURE_ROOT
						if ( _closure(target)->_root &&
							( _table(_closure(target)->_root->_obj) != _table(m_pRootVM->_roottable) ) )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("root") );
							elem.SetString( "value", GetValue( _closure(target)->_root->_obj ) );
							elem.SetInt( "variablesReference", ToVarRef( _closure(target)->_root->_obj ) );
							SetVirtualHint( elem );
						}
#endif
						break;
					}
					case OT_NATIVECLOSURE:
					{
						if ( sq_type( _nativeclosure(target)->_name ) != OT_NULL )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("name") );
							elem.SetString( "value", GetValue( _nativeclosure(target)->_name ) );
							elem.SetInt( "variablesReference", -1 );
							SetVirtualHint( elem );
						}

						if ( _nativenoutervalues( _nativeclosure(target) ) )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("outervalues") );
							elem.SetString( "value", GetValue(
										_nativenoutervalues( _nativeclosure(target) ), flags ) );
							elem.SetInt( "variablesReference", -1 );
							SetVirtualHint( elem );
						}
#ifdef CLOSURE_ENV
						if ( CLOSURE_ENV_ISVALID( _nativeclosure(target)->_env ) )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("env") );
							elem.SetString( "value", GetValue(
										CLOSURE_ENV_OBJ( _nativeclosure(target)->_env ) ) );
							elem.SetInt( "variablesReference", ToVarRef(
										CLOSURE_ENV_OBJ( _nativeclosure(target)->_env ) ) );
							SetVirtualHint( elem );
						}
#endif
						break;
					}
					case OT_THREAD:
					{
						Assert( _thread(_ss(_thread(target))->_root_vm) == m_pRootVM );

						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("state") );
							switch ( sq_getvmstate( _thread(target) ) )
							{
								case SQ_VMSTATE_IDLE:		elem.SetString( "value", "idle" ); break;
								case SQ_VMSTATE_RUNNING:	elem.SetString( "value", "running" ); break;
								case SQ_VMSTATE_SUSPENDED:	elem.SetString( "value", "suspended" ); break;
								default: UNREACHABLE();
							}
							elem.SetInt( "variablesReference", -1 );
							SetVirtualHint( elem );
						}

						if ( _table(_thread(target)->_roottable) != _table(m_pRootVM->_roottable) )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("root") );
							elem.SetString( "value", GetValue( _thread(target)->_roottable ) );
							elem.SetInt( "variablesReference", -1 );
							SetVirtualHint( elem );
						}

						if ( _thread(target) != m_pRootVM )
						{
							const SQObjectPtr &val = _thread(target)->_stack._vals[0];
							Assert( sq_type(val) == OT_CLOSURE || sq_type(val) == OT_NATIVECLOSURE );

							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("function") );
							elem.SetString( "value", GetValue( val ) );
							elem.SetInt( "variablesReference", ToVarRef( val ) );
							SetVirtualHint( elem );
						}

						if ( _thread(target)->_callsstacksize != 0 )
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("callstack") );
							elem.SetString( "value", GetValue( _thread(target)->_callsstacksize ) );
							elem.SetInt( "variablesReference", ToVarRef( VARREF_CALLSTACK, target ) );
							SetVirtualHint( elem );
						}

						break;
					}
					case OT_GENERATOR:
					{
						{
							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("state") );
							switch ( _generator(target)->_state )
							{
								case SQGenerator::eSuspended:	elem.SetString( "value", "suspended" ); break;
								case SQGenerator::eRunning:		elem.SetString( "value", "running" ); break;
								case SQGenerator::eDead:		elem.SetString( "value", "dead" ); break;
								default: UNREACHABLE();
							}
							elem.SetInt( "variablesReference", -1 );
							SetVirtualHint( elem );
						}

						if ( _generator(target)->_state != SQGenerator::eDead )
						{
							const SQVM::CallInfo &ci = _generator(target)->_ci;
							Assert( sq_type(ci._closure) == OT_CLOSURE );

							SQFunctionProto *func = _fp(_closure(ci._closure)->_function);

							const SQChar *source = _SC("unknown");
							if ( sq_type(func->_sourcename) == OT_STRING )
								source = _stringval(func->_sourcename);

							int line = func->GetLine( ci._ip );

							stringbuf_t< 256 > buf;
							buf.Puts( sqstring_t( source, scstrlen(source) ) );
							buf.Put(':');
							buf.PutInt( line );

							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("frame") );
							elem.SetString( "value", buf );
							elem.SetInt( "variablesReference", -1 );
							SetVirtualHint( elem );
						}

						if ( sq_type(_generator(target)->_closure) != OT_NULL )
						{
							const SQObjectPtr &val = _generator(target)->_closure;
							Assert( sq_type(val) == OT_CLOSURE || sq_type(val) == OT_NATIVECLOSURE );

							json_table_t &elem = variables.AppendTable(4);
							elem.SetString( "name", INTERNAL_TAG("function") );
							elem.SetString( "value", GetValue( val ) );
							elem.SetInt( "variablesReference", ToVarRef( val ) );
							SetVirtualHint( elem );
						}

						break;
					}
					case OT_STRING:
					case OT_ARRAY:
					case OT_WEAKREF:
						break;
					default:
						Assert(!"unknown type");
				}
			DAP_SEND();
			break;
		}
		case VARREF_INSTRUCTIONS:
		{
			if ( sq_type( ref->GetVar() ) != OT_CLOSURE )
			{
				DAP_ERROR_RESPONSE( seq, "variables" );
				DAP_ERROR_BODY( 0, "invalid object", 0 );
				DAP_SEND();
				return;
			}

			int ninstructions = _fp(_closure(ref->GetVar())->_function)->_ninstructions;
			SQFunctionProto *func = _fp(_closure(ref->GetVar())->_function);

			DAP_START_RESPONSE( seq, "variables" );
			DAP_SET_TABLE( body, 1 );
				json_array_t &variables = body.SetArray( "variables", ninstructions );

				// ignore line ops
				int lines = 0;

				for ( int i = 0; i < ninstructions; i++ )
				{
					SQInstruction *instr = func->_instructions + i;

					if ( instr->op == _OP_LINE )
					{
						lines++;
						continue;
					}

					json_table_t &elem = variables.AppendTable(4);
					{
						stringbuf_t< 32 > instrBytes; // "0xFF -2147483648 255 255 255"
						instrBytes.PutHex( instr->op ); instrBytes.Put(' ');
						instrBytes.PutInt( instr->_arg0 ); instrBytes.Put(' ');
						instrBytes.PutInt( instr->_arg1 ); instrBytes.Put(' ');
						instrBytes.PutInt( instr->_arg2 ); instrBytes.Put(' ');
						instrBytes.PutInt( instr->_arg3 );
						elem.SetString( "value", instrBytes );
					}
					{
						stringbuf_t< 64 > name; // index:line
						name.PutInt( i - lines );
						name.Put(':');
						name.PutInt( func->GetLine( instr ) );
						elem.SetString( "name", name );
					}
					elem.SetInt( "variablesReference", -1 );
#ifndef SUPPORTS_SET_INSTRUCTION
					elem.SetTable( "presentationHint", 1 ).SetArray( "attributes", 1 ).Append( "readOnly" );
#endif
				}
			DAP_SEND();

			break;
		}
		case VARREF_OUTERS:
		{
			if ( sq_type( ref->GetVar() ) != OT_CLOSURE )
			{
				DAP_ERROR_RESPONSE( seq, "variables" );
				DAP_ERROR_BODY( 0, "invalid object", 0 );
				DAP_SEND();
				return;
			}

			SQClosure *pClosure = _closure(ref->GetVar());
			SQFunctionProto *func = _fp(pClosure->_function);

			DAP_START_RESPONSE( seq, "variables" );
			DAP_SET_TABLE( body, 1 );
				json_array_t &variables = body.SetArray( "variables", func->_noutervalues );
				for ( int i = func->_noutervalues; i--; )
				{
					const SQOuterVar &var = func->_outervalues[i];
					Assert( sq_type(var._name) == OT_STRING );

					const SQObjectPtr &val = *_outervalptr( pClosure->_outervalues[i] );
					json_table_t &elem = variables.AppendTable(4);
					elem.SetString( "name", sqstring_t( _string(var._name) ) );
					elem.SetString( "value", GetValue( val, flags ) );
					elem.SetString( "type", GetType( val ) );
					elem.SetInt( "variablesReference", ToVarRef( val ) );
				}
			DAP_SEND();

			break;
		}
		case VARREF_LITERALS:
		{
			if ( sq_type( ref->GetVar() ) != OT_CLOSURE )
			{
				DAP_ERROR_RESPONSE( seq, "variables" );
				DAP_ERROR_BODY( 0, "invalid object", 0 );
				DAP_SEND();
				return;
			}

			SQClosure *pClosure = _closure(ref->GetVar());
			SQFunctionProto *func = _fp(pClosure->_function);

			DAP_START_RESPONSE( seq, "variables" );
			DAP_SET_TABLE( body, 1 );
				json_array_t &variables = body.SetArray( "variables", func->_nliterals );
				for ( int i = 0; i < func->_nliterals; i++ )
				{
					const SQObjectPtr &lit = func->_literals[i];

					json_table_t &elem = variables.AppendTable(4);
					elem.SetString( "name", GetValue( i ) );
					elem.SetString( "value", GetValue( lit, flags ) );
					elem.SetString( "type", GetType( lit ) );
					elem.SetInt( "variablesReference", ToVarRef( lit ) );
				}
			DAP_SEND();

			break;
		}
		case VARREF_METAMETHODS:
		{
			SQObject target = ref->GetVar();

			if ( !ISREFCOUNTED( sq_type(target) ) )
			{
				DAP_ERROR_RESPONSE( seq, "variables" );
				DAP_ERROR_BODY( 0, "invalid object", 0 );
				DAP_SEND();
				return;
			}

			DAP_START_RESPONSE( seq, "variables" );
			DAP_SET_TABLE( body, 1 );
				json_array_t &variables = body.SetArray( "variables" );
				switch ( sq_type(target) )
				{
					case OT_INSTANCE: target._unVal.pClass = _instance(target)->_class;
					case OT_CLASS:
					{
						for ( unsigned int i = 0; i < MT_LAST; i++ )
						{
							const SQObjectPtr &val = _class(target)->_metamethods[i];
							if ( sq_type( _class(target)->_metamethods[i] ) != OT_NULL )
							{
								json_table_t &elem = variables.AppendTable(4);
								elem.SetString( "name", g_MetaMethodName[i] );
								elem.SetString( "value", GetValue( val, 0 ) );
								elem.SetString( "type", GetType( val ) );
								elem.SetInt( "variablesReference", ToVarRef( val ) );
							}
						}

						break;
					}
					default:
					{
						Assert( is_delegable(target) && _delegable(target)->_delegate );

						if ( is_delegable(target) && _delegable(target)->_delegate )
						{
							SQObjectPtr val;
							for ( unsigned int i = 0; i < MT_LAST; i++ )
							{
								if ( _delegable(target)->GetMetaMethod( m_pRootVM, (SQMetaMethod)i, val ) )
								{
									json_table_t &elem = variables.AppendTable(4);
									elem.SetString( "name", g_MetaMethodName[i] );
									elem.SetString( "value", GetValue( val, 0 ) );
									elem.SetString( "type", GetType( val ) );
									elem.SetInt( "variablesReference", ToVarRef( val ) );
								}
							}
						}

						break;
					}
				}
			DAP_SEND();

			break;
		}
		case VARREF_CALLSTACK:
		{
			SQObject target = ref->GetVar();

			if ( sq_type(target) != OT_THREAD )
			{
				DAP_ERROR_RESPONSE( seq, "variables" );
				DAP_ERROR_BODY( 0, "invalid object", 0 );
				DAP_SEND();
				return;
			}

			DAP_START_RESPONSE( seq, "variables" );
			DAP_SET_TABLE( body, 1 );
				int i = _thread(target)->_callsstacksize;
				json_array_t &variables = body.SetArray( "variables", i );
				{
					while ( i-- )
					{
						const SQVM::CallInfo &ci = _thread(target)->_callsstack[i];

						if ( ShouldIgnoreStackFrame(ci) )
							continue;

						const SQChar *source = _SC("unknown");
						int line = -1;

						if ( sq_type(ci._closure) == OT_CLOSURE )
						{
							SQFunctionProto *func = _fp(_closure(ci._closure)->_function);

							if ( sq_type(func->_sourcename) == OT_STRING )
								source = _stringval(func->_sourcename);

							line = func->GetLine( ci._ip );
						}
						else if ( sq_type(ci._closure) == OT_NATIVECLOSURE )
						{
							source = _SC("NATIVE");
						}

						stringbuf_t< 256 > buf;
						buf.Puts( sqstring_t( source, scstrlen(source) ) );
						buf.Put(':');
						buf.PutInt( line );

						json_table_t &elem = variables.AppendTable(3);
						elem.SetString( "name", GetValue( i ) );
						elem.SetString( "value", buf );
						elem.SetInt( "variablesReference", ToVarRef( ci._closure ) );
					}
				}
			DAP_SEND();

			break;
		}
		default: UNREACHABLE();
	}
}

//
// If the client supports SetExpression and the target variable has "evaluateName",
// it will send target "evaluateName" and stack frame to SetExpression.
// Client can choose to send variable "name" to SetExpression for watch variables.
// If the client doesn't support SetExpression or the target variable does not have "evaluateName",
// it will send target "name" and container "variableReference" to SetVariable.
//
// SetExpression needs to parse watch flags and "evaluateName",
// SetVariable only gets identifiers.
//
void SQDebugServer::OnRequest_SetVariable( const json_table_t &arguments, int seq )
{
	int variablesReference;
	string_t strName, strValue;

	arguments.GetInt( "variablesReference", &variablesReference );
	arguments.GetString( "name", &strName );
	arguments.GetString( "value", &strValue );

	bool hex = false;
	json_table_t *format;
	if ( arguments.GetTable( "format", &format ) )
		format->GetBool( "hex", &hex );

	varref_t *ref = FromVarRef( variablesReference );
	if ( !ref )
	{
		DAP_ERROR_RESPONSE( seq, "setVariable" );
		DAP_ERROR_BODY( 0, "failed to find variable", 0 );
		DAP_SEND();
		return;
	}

	if ( strValue.IsEmpty() )
	{
		DAP_ERROR_RESPONSE( seq, "evaluate" );
		DAP_ERROR_BODY( 0, "empty expression", 0 );
		DAP_SEND();
		return;
	}

	switch ( ref->type )
	{
		// Requires special value parsing
		case VARREF_INSTRUCTIONS:
		{
#ifndef SUPPORTS_SET_INSTRUCTION
			DAP_ERROR_RESPONSE( seq, "setVariable" );
			DAP_ERROR_BODY( 0, "not supported", 0 );
			DAP_SEND();
#else
			if ( sq_type( ref->GetVar() ) != OT_CLOSURE )
			{
				DAP_ERROR_RESPONSE( seq, "setVariable" );
				DAP_ERROR_BODY( 0, "invalid object" );
				DAP_SEND();
				return;
			}

			int index, line;
			int op, arg0, arg1, arg2, arg3;

			int c1 = sscanf( strName.ptr, "%d:%d", &index, &line );
			int c2 = sscanf( strValue.ptr, "0x%02x %d %d %d %d", &op, &arg0, &arg1, &arg2, &arg3 );

			bool fail = ( c1 != 2 );

			if ( !fail && c2 != 5 )
			{
				// Check for floats
				if ( strchr( strValue.ptr, '.' ) )
				{
					float farg1;
					c2 = sscanf( strValue.ptr, "0x%02x %d %f %d %d", &op, &arg0, &farg1, &arg2, &arg3 );
					if ( c2 != 5 )
					{
						fail = true;
					}
					else
					{
						arg1 = *(int*)&farg1;

						if ( op != _OP_LOADFLOAT )
						{
							PrintError(_SC("Warning: Setting float value (%.2f) to non-float instruction\n"), farg1);
						}
					}
				}
				else
				{
					fail = true;
				}
			}

			if ( fail )
			{
				DAP_ERROR_RESPONSE( seq, "setVariable" );
				DAP_ERROR_BODY( 0, "invalid amount of parameters in input", 1 );
				error.SetBool( "showUser", true );
				DAP_SEND();
				return;
			}

			SQFunctionProto *func = _fp(_closure(ref->GetVar())->_function);

			// line ops are ignored in the index
			for ( int c = 0; c < func->_ninstructions; c++ )
			{
				if ( func->_instructions[c].op == _OP_LINE )
					index++;

				if ( c == index )
					break;
			}

			Assert( index < func->_ninstructions );
			SQInstruction *instr = func->_instructions + index;

			instr->op = op & 0xff;
			instr->_arg0 = arg0 & 0xff;
			instr->_arg1 = arg1;
			instr->_arg2 = arg2 & 0xff;
			instr->_arg3 = arg3 & 0xff;

			stringbuf_t< 32 > instrBytes;
			instrBytes.PutHex( instr->op ); instrBytes.Put(' ');
			instrBytes.PutInt( instr->_arg0 ); instrBytes.Put(' ');
			instrBytes.PutInt( instr->_arg1 ); instrBytes.Put(' ');
			instrBytes.PutInt( instr->_arg2 ); instrBytes.Put(' ');
			instrBytes.PutInt( instr->_arg3 );

			DAP_START_RESPONSE( seq, "setVariable" );
			DAP_SET_TABLE( body, 2 );
				body.SetString( "value", instrBytes );
				body.SetInt( "variablesReference", -1 );
			DAP_SEND();
#endif
			return;
		}
		default: break;
	}

	objref_t obj;

	if ( !GetObj( strName, ref, obj ) )
	{
		DAP_ERROR_RESPONSE( seq, "setVariable" );
		DAP_ERROR_BODY( 0, "identifier '{name}' not found", 2 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "name", strName );
		error.SetBool( "showUser", true );
		DAP_SEND();
		return;
	}

	SQObjectPtr value;

	if ( IsScopeRef( ref->type ) )
	{
		if ( !RunExpression( strValue, ref->GetThread(), ref->scope.stack, value ) )
		{
			DAP_ERROR_RESPONSE( seq, "setVariable" );
			DAP_ERROR_BODY( 0, "failed to evaluate value '{name}'\n\n[{reason}]", 2 );
			json_table_t &variables = error.SetTable( "variables", 2 );
			variables.SetString( "name", strValue );
			variables.SetString( "reason", GetValue( ref->GetThread()->_lasterror, NO_QUOT ) );
			error.SetBool( "showUser", true );
			DAP_SEND();
			return;
		}
	}
	else
	{
		if ( !RunExpression( strValue, m_pCurVM, -1, value ) )
		{
			DAP_ERROR_RESPONSE( seq, "setVariable" );
			DAP_ERROR_BODY( 0, "failed to evaluate value '{name}'\n\n[{reason}]", 2 );
			json_table_t &variables = error.SetTable( "variables", 2 );
			variables.SetString( "name", strValue );
			variables.SetString( "reason", GetValue( m_pCurVM->_lasterror, NO_QUOT ) );
			error.SetBool( "showUser", true );
			DAP_SEND();
			return;
		}
	}

	if ( !Set( obj, value ) )
	{
		DAP_ERROR_RESPONSE( seq, "setVariable" );
		DAP_ERROR_BODY( 0, "could not set '{name}'", 1 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "name", GetValue( obj.key ) );
		DAP_SEND();
		return;
	}

	// Update data watch
	if ( IsObjectRef( ref->type ) && ref->obj.isWeakref &&
			_refcounted( ref->GetVar() )->_weakref )
	{
		for ( int i = m_DataWatches.size(); i--; )
		{
			datawatch_t &dw = m_DataWatches[i];
			if ( _refcounted( ref->GetVar() )->_weakref == dw.container &&
					dw.name.IsEqualTo( strName ) )
			{
				dw.oldvalue = value;
				break;
			}
		}
	}

	DAP_START_RESPONSE( seq, "setVariable" );
	DAP_SET_TABLE( body, 3 );
		body.SetString( "value", GetValue( value, hex ? HEX : 0 ) );
		body.SetString( "type", GetType( value ) );
		body.SetInt( "variablesReference", ToVarRef( value ) );
	DAP_SEND();
}

void SQDebugServer::OnRequest_SetExpression( const json_table_t &arguments, int seq )
{
	HSQUIRRELVM vm;
	int frame;

	string_t expression, strValue;

	arguments.GetString( "expression", &expression );
	arguments.GetString( "value", &strValue );
	arguments.GetInt( "frameId", &frame, -1 );

	TranslateFrameID( frame, &vm, &frame );

	int flags = ParseFormatSpecifiers( expression );
	{
		json_table_t *format;
		if ( arguments.GetTable( "format", &format ) )
		{
			bool hex;
			format->GetBool( "hex", &hex );
			if ( hex )
				flags |= HEX;
		}
	}

	objref_t obj;
	obj.type = objref_t::INVALID;

	SQObjectPtr value;

	if ( !( (flags & BIN) && ParseBinaryNumber( strValue, value ) ) &&
			!RunExpression( strValue, vm, frame, value ) )
	{
		DAP_ERROR_RESPONSE( seq, "setExpression" );
		DAP_ERROR_BODY( 0, "failed to evaluate value '{name}'", 2 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "name", strValue );
		error.SetBool( "showUser", true );
		DAP_SEND();
		return;
	}

	// Try to get identifier
	if ( ShouldParseEvaluateName( expression ) )
	{
		if ( !ParseEvaluateName( expression, vm, frame, obj ) )
		{
			DAP_ERROR_RESPONSE( seq, "setExpression" );
			DAP_ERROR_BODY( 0, "invalid variable reference '{name}'", 2 );
			json_table_t &variables = error.SetTable( "variables", 1 );
			variables.SetString( "name", expression );
			error.SetBool( "showUser", true );
			DAP_SEND();
			return;
		}
	}
	else
	{
		if ( flags & LOCK )
		{
			int foundWatch = 0;

			for ( unsigned int i = 0; i < m_LockedWatches.size(); i++ )
			{
				watch_t &w = m_LockedWatches[i];
				if ( w.expression.IsEqualTo( expression ) )
				{
					vm = GetThread( w.thread );
					frame = w.frame;
					foundWatch = 1;
					break;
				}
			}

			Assert( foundWatch );
		}

		GetObj( expression, vm, frame, obj );
	}

	// Found identifier
	if ( obj.type != objref_t::INVALID )
	{
		if ( Set( obj, value ) )
		{
			DAP_START_RESPONSE( seq, "setExpression" );
			DAP_SET_TABLE( body, 3 );
				body.SetString( "value", GetValue( value, flags ) );
				body.SetString( "type", GetType( value ) );
				body.SetInt( "variablesReference", ToVarRef( value ) );
			DAP_SEND();
		}
		else
		{
			DAP_ERROR_RESPONSE( seq, "setExpression" );
			DAP_ERROR_BODY( 0, "could not set '{name}'", 1 );
			json_table_t &variables = error.SetTable( "variables", 1 );
			variables.SetString( "name", GetValue( obj.key ) );
			DAP_SEND();
		}
	}
	// No identifier, run the script ( exp = val )
	else
	{
		stringbuf_t< 256 > buf;
		int len = expression.len + 1;

		if ( !(flags & BIN) )
		{
			len += strValue.len;
		}
		else
		{
			len += counthexdigits( (SQUnsignedInteger)_integer(value) );
		}

		if ( len < sizeof(buf.ptr) )
		{
			buf.Puts( expression );
			buf.Put('=');

			if ( !(flags & BIN) )
			{
				buf.Puts( strValue );
			}
			else
			{
				buf.PutHex( (SQUnsignedInteger)_integer(value), false );
			}

			buf.Term();

			if ( RunExpression( buf, vm, frame, value ) )
			{
				DAP_START_RESPONSE( seq, "setExpression" );
				DAP_SET_TABLE( body, 3 );
					body.SetString( "value", GetValue( value, flags ) );
					body.SetString( "type", GetType( value ) );
					body.SetInt( "variablesReference", ToVarRef( value ) );
				DAP_SEND();
			}
			else
			{
				DAP_ERROR_RESPONSE( seq, "setExpression" );
				DAP_ERROR_BODY( 0, "failed to evaluate expression: {exp} = {val}\n\n[{reason}]", 2 );
				json_table_t &variables = error.SetTable( "variables", 3 );
				variables.SetString( "exp", expression );
				variables.SetString( "val", strValue );
				variables.SetString( "reason", GetValue( vm->_lasterror, NO_QUOT ) );
				error.SetBool( "showUser", true );
				DAP_SEND();
			}
		}
		else
		{
			DAP_ERROR_RESPONSE( seq, "setExpression" );
			DAP_ERROR_BODY( 0, "expression is too long to evaluate: {exp} = {val}", 2 );
			json_table_t &variables = error.SetTable( "variables", 2 );
			variables.SetString( "exp", expression );
			variables.SetString( "val", strValue );
			error.SetBool( "showUser", true );
			DAP_SEND();
		}
	}
}

void SQDebugServer::OnRequest_Disassemble( const json_table_t &arguments, int seq )
{
	string_t memoryReference;
	int instructionOffset, instructionCount;

	arguments.GetString( "memoryReference", &memoryReference );
	arguments.GetInt( "instructionOffset", &instructionOffset );
	arguments.GetInt( "instructionCount", &instructionCount );

	SQFunctionProto *func = NULL;
	int instrIdx = -1;

	SQInstruction *ip;
	atox( memoryReference, (SQUnsignedInteger*)&ip );

	for ( int i = m_pCurVM->_callsstacksize; i--; )
	{
		const SQVM::CallInfo &ci = m_pCurVM->_callsstack[i];
		if ( sq_type(ci._closure) == OT_CLOSURE )
		{
			if ( ci._ip == ip )
			{
				func = _fp(_closure(ci._closure)->_function);
				instrIdx = ci._ip - func->_instructions;
				break;
			}
		}
	}

	if ( instrIdx == -1 || instrIdx >= func->_ninstructions )
	{
		DAP_ERROR_RESPONSE( seq, "disassemble" );
		DAP_ERROR_BODY( 0, "invalid instruction pointer", 0 );
		DAP_SEND();
		return;
	}

	int targetStart = instrIdx + instructionOffset;
	int targetEnd = targetStart + instructionCount;

	int validStart = max( 0, targetStart );
	int validEnd = min( func->_ninstructions - 1, targetEnd );

	DAP_START_RESPONSE( seq, "disassemble" );
	DAP_SET_TABLE( body, 1 );
		json_array_t &instructions = body.SetArray( "instructions", instructionCount );

		for ( int index = targetStart; index < targetEnd; index++ )
		{
			json_table_t &elem = instructions.AppendTable(5);

			SQInstruction *instr = func->_instructions + index;

			stringbuf_t< FMT_PTR_LEN + 1 > addr;
			addr.PutHex( (SQUnsignedInteger)instr );
			elem.SetString( "address", addr );

			if ( index >= validStart && index <= validEnd )
			{
				if ( instr->op != _OP_LINE )
				{
					{
						stringbuf_t< 128 > instrStr;
						instrStr.len = DescribeInstruction( instr, func, instrStr.ptr, sizeof(instrStr.ptr) );
						elem.SetString( "instruction", instrStr );
					}
					{
						stringbuf_t< 32 > instrBytes;
						instrBytes.PutHex( instr->op ); instrBytes.Put(' ');
						instrBytes.PutInt( instr->_arg0 ); instrBytes.Put(' ');
						instrBytes.PutInt( instr->_arg1 ); instrBytes.Put(' ');
						instrBytes.PutInt( instr->_arg2 ); instrBytes.Put(' ');
						instrBytes.PutInt( instr->_arg3 );
						elem.SetString( "instructionBytes", instrBytes );
					}

					elem.SetInt( "line", (int)func->GetLine( instr ) );
				}
				else
				{
					elem.SetString( "instruction", "" );
				}

				elem.SetString( "presentationHint", "normal" );
			}
			else
			{
				elem.SetString( "instruction", "??" );
				elem.SetString( "presentationHint", "invalid" );
			}

			if ( index == targetStart )
			{
				json_table_t &source = elem.SetTable( "location", 2 );
				source.SetString( "name", sqstring_t( _string(func->_sourcename) ) );
				sqstring_t *path = m_FilePathMap.Get( _string(func->_sourcename) );
				if ( path )
					source.SetString( "path", *path );
			}
		}

		Assert( (int)instructions.size() == instructionCount );
	DAP_SEND();
}

#ifdef SUPPORTS_RESTART_FRAME
void SQDebugServer::OnRequest_RestartFrame( const json_table_t &arguments, int seq )
{
	Assert( m_State == ThreadState::Suspended );

	HSQUIRRELVM vm;
	int frame;
	arguments.GetInt( "frameId", &frame, -1 );

	if ( !TranslateFrameID( frame, &vm, &frame ) )
	{
		DAP_ERROR_RESPONSE( seq, "restartFrame" );
		DAP_ERROR_BODY( 0, "invalid stack frame {id}", 1 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "id", GetValue( frame ) );
		DAP_SEND();
		return;
	}

	if ( vm != m_pCurVM )
	{
		DAP_ERROR_RESPONSE( seq, "restartFrame" );
		DAP_ERROR_BODY( 0, "cannot restart frame on a different thread", 0 );
		DAP_SEND();
		return;
	}

	int curframe = GetCurrentStackFrame( vm );

	for ( int f = curframe; f >= frame; f-- )
	{
		if ( sq_type( vm->_callsstack[f]._closure ) != OT_CLOSURE )
		{
			DAP_ERROR_RESPONSE( seq, "restartFrame" );
			DAP_ERROR_BODY( 0, "cannot restart native call frame", 0 );
			DAP_SEND();
			return;
		}
	}

	while ( curframe > frame )
	{
		vm->LeaveFrame();
		curframe = GetCurrentStackFrame( vm );
	}

	Assert( vm->ci == &vm->_callsstack[ curframe ] );
	Assert( sq_type(vm->ci->_closure) == OT_CLOSURE );

	SQFunctionProto *func = _fp(_closure(vm->ci->_closure)->_function);

	vm->ci->_ip = func->_instructions;

	int top = vm->_top;
	int target = top - func->_stacksize + func->_nparameters;

	while ( top --> target )
	{
		vm->_stack._vals[top].Null();
	}

	DAP_START_RESPONSE( seq, "restartFrame" );
	DAP_SEND();

	m_BreakReason.reason = BreakReason::Restart;
	Break( vm );
}
#endif

static inline int GetOpAtLine( SQFunctionProto *func, int line )
{
	for ( int i = 0; i < func->_nlineinfos; i++ )
	{
		const SQLineInfo &li = func->_lineinfos[i];
		if ( line <= li._line )
			return li._op;
	}

	return -1;
}

//
// Only allow goto if the requested source,line matches currently executing function
//
void SQDebugServer::OnRequest_GotoTargets( const json_table_t &arguments, int seq )
{
	if ( m_State != ThreadState::Suspended )
	{
		DAP_ERROR_RESPONSE( seq, "gotoTargets" );
		DAP_ERROR_BODY( 0, "thread is not suspended", 0 );
		DAP_SEND();
		return;
	}

	json_table_t *source;
	string_t srcname( (char*)0, 0 );
	int line;

	if ( arguments.GetTable( "source", &source ) )
		source->GetString( "name", &srcname );

	arguments.GetInt( "line", &line );

#ifdef NATIVE_DEBUG_HOOK
	SQVM::CallInfo *ci = m_pCurVM->ci;
#else
	SQVM::CallInfo *ci = m_pCurVM->ci - 1;
#endif

	if ( !ci )
	{
		DAP_ERROR_RESPONSE( seq, "gotoTargets" );
		DAP_ERROR_BODY( 0, "thread is not in execution", 0 );
		DAP_SEND();
		return;
	}

	if ( sq_type( ci->_closure ) != OT_CLOSURE )
	{
		DAP_ERROR_RESPONSE( seq, "gotoTargets" );
		DAP_ERROR_BODY( 0, "goto on native call frame", 0 );
		DAP_SEND();
		return;
	}

	SQFunctionProto *func = _fp(_closure(ci->_closure)->_function);

	if ( func->_nlineinfos == 0 )
	{
		DAP_ERROR_RESPONSE( seq, "gotoTargets" );
		DAP_ERROR_BODY( 0, "no lineinfos", 0 );
		DAP_SEND();
		return;
	}

	if ( sq_type(func->_sourcename) == OT_STRING )
	{
		sqstring_t wfuncsrc( _string(func->_sourcename) );

#ifdef SOURCENAME_HAS_PATH
		for ( SQChar *c = wfuncsrc.ptr + wfuncsrc.len - 1; c > wfuncsrc.ptr; c-- )
		{
			if ( *c == '/' || *c == '\\' )
			{
				c++;
				wfuncsrc.len = wfuncsrc.ptr + wfuncsrc.len - c;
				wfuncsrc.ptr = c;
				break;
			}
		}
#endif

		stringbuf_t< 256 > funcsrc;
		funcsrc.Puts( wfuncsrc );

		if ( !srcname.IsEqualTo( funcsrc ) )
		{
			DAP_ERROR_RESPONSE( seq, "gotoTargets" );
			DAP_ERROR_BODY( 0, "requested source '{src1}' does not match executing function '{src2}'", 2 );
			json_table_t &variables = error.SetTable( "variables", 2 );
			variables.SetString( "src1", srcname );
			variables.SetString( "src2", funcsrc );
			error.SetBool( "showUser", true );
			DAP_SEND();
			return;
		}
	}

	if ( line < 0 || line > func->_lineinfos[func->_nlineinfos-1]._line )
	{
		DAP_ERROR_RESPONSE( seq, "gotoTargets" );
		DAP_ERROR_BODY( 0, "line {line} is out of range", 2 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "line", GetValue( line ) );
		error.SetBool( "showUser", true );
		DAP_SEND();
		return;
	}

	int op = GetOpAtLine( func, line );

	if ( op == -1 )
	{
		DAP_ERROR_RESPONSE( seq, "gotoTargets" );
		DAP_ERROR_BODY( 0, "could not find line {line} in function", 2 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "line", GetValue( line ) );
		error.SetBool( "showUser", true );
		DAP_SEND();
		return;
	}

	DAP_START_RESPONSE( seq, "gotoTargets" );
	DAP_SET_TABLE( body, 1 );
		json_array_t &targets = body.SetArray( "targets", 1 );
			json_table_t &elem = targets.AppendTable(3);
			stringbuf_t< 1 + FMT_INT_LEN + 1 > buf;
			buf.Put('L'); buf.PutInt( line );
			elem.SetString( "label", buf );
			elem.SetInt( "line", line );
			elem.SetInt( "id", line );
	DAP_SEND();
}

void SQDebugServer::OnRequest_Goto( const json_table_t &arguments, int seq )
{
	if ( m_State != ThreadState::Suspended )
	{
		DAP_ERROR_RESPONSE( seq, "goto" );
		DAP_ERROR_BODY( 0, "thread is not suspended", 0 );
		DAP_SEND();
		return;
	}

	int threadId, line;
	arguments.GetInt( "threadId", &threadId );
	arguments.GetInt( "targetId", &line );

	HSQUIRRELVM vm = ThreadFromID( threadId );
	if ( !vm )
	{
		DAP_ERROR_RESPONSE( seq, "goto" );
		DAP_ERROR_BODY( 0, "thread is dead", 0 );
		DAP_SEND();
		return;
	}

	if ( vm != m_pCurVM )
	{
		DAP_ERROR_RESPONSE( seq, "goto" );
		DAP_ERROR_BODY( 0, "cannot change execution on a different thread", 0 );
		DAP_SEND();
		return;
	}

#ifdef NATIVE_DEBUG_HOOK
	SQVM::CallInfo *ci = vm->ci;
#else
	SQVM::CallInfo *ci = vm->ci - 1;
#endif

	if ( !ci )
	{
		DAP_ERROR_RESPONSE( seq, "goto" );
		DAP_ERROR_BODY( 0, "thread is not in execution", 0 );
		DAP_SEND();
		return;
	}

	if ( sq_type( ci->_closure ) != OT_CLOSURE )
	{
		DAP_ERROR_RESPONSE( seq, "goto" );
		DAP_ERROR_BODY( 0, "goto on native call frame", 0 );
		DAP_SEND();
		return;
	}

	SQFunctionProto *func = _fp(_closure(ci->_closure)->_function);

	if ( func->_nlineinfos == 0 )
	{
		DAP_ERROR_RESPONSE( seq, "goto" );
		DAP_ERROR_BODY( 0, "no lineinfos", 0 );
		DAP_SEND();
		return;
	}

	if ( line < 0 || line > func->_lineinfos[func->_nlineinfos-1]._line )
	{
		DAP_ERROR_RESPONSE( seq, "goto" );
		DAP_ERROR_BODY( 0, "line {line} is out of range", 2 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "line", GetValue( line ) );
		error.SetBool( "showUser", true );
		DAP_SEND();
		return;
	}

	int op = GetOpAtLine( func, line );

	if ( op == -1 )
	{
		DAP_ERROR_RESPONSE( seq, "goto" );
		DAP_ERROR_BODY( 0, "could not find line {line} in function", 2 );
		json_table_t &variables = error.SetTable( "variables", 1 );
		variables.SetString( "line", GetValue( line ) );
		error.SetBool( "showUser", true );
		DAP_SEND();
		return;
	}

	ci->_ip = func->_instructions + op;

	if ( ci->_ip->op == _OP_LINE && ci->_ip + 1 < func->_instructions + func->_ninstructions )
		ci->_ip++;

	DAP_START_RESPONSE( seq, "goto" );
	DAP_SEND();

	m_BreakReason.reason = BreakReason::Goto;
	Break( vm );
}

int SQDebugServer::ToVarRef( EVARREF type, HSQUIRRELVM vm, int stack )
{
	Assert( IsScopeRef( type ) );

	SQWeakRef *thread = GetWeakRef( vm );

	for ( unsigned int i = 0; i < m_Vars.size(); i++ )
	{
		varref_t &v = m_Vars[i];
		if ( v.type == type && v.scope.stack == stack && v.scope.thread == thread )
			return v.id;
	}

	varref_t &var = m_Vars.push_back();
	var.id = m_nVarRefIndex++;
	var.type = type;
	var.scope.stack = stack;
	var.scope.thread = thread;

	return var.id;
}

void SQDebugServer::ConvertToWeakRef( varref_t &v )
{
	Assert( IsObjectRef( v.type ) );

	if ( !v.obj.isWeakref )
	{
		v.obj.weakref = GetWeakRef( _refcounted(v.obj.obj), sq_type(v.obj.obj) );
		__ObjAddRef( v.obj.weakref );
		v.obj.isWeakref = true;
	}
}

int SQDebugServer::ToVarRef( EVARREF type, const SQObject &obj, bool isWeakref )
{
	Assert( IsObjectRef( type ) );

	if ( !ISREFCOUNTED( sq_type(obj) ) )
		return -1;

	if ( sq_type(obj) == OT_WEAKREF && isWeakref )
	{
		if ( sq_type( _weakref(obj)->_obj ) == OT_NULL )
			return -1;
	}

	for ( int i = m_Vars.size(); i--; )
	{
		varref_t &v = m_Vars[i];
		if ( v.type == type && _rawval( v.GetVar() ) == _rawval( obj ) )
		{
			if ( isWeakref )
				ConvertToWeakRef( v );

			// this is slow!
			v.obj.hasNonStringMembers = HasNonStringMembers( obj );

			return v.id;
		}
	}

	varref_t *var = &m_Vars.push_back();
	var->id = m_nVarRefIndex++;
	var->type = type;
	var->obj.isWeakref = isWeakref;

	if ( isWeakref )
	{
		var->obj.weakref = GetWeakRef( _refcounted(obj), sq_type(obj) );
		__ObjAddRef( var->obj.weakref );

		Assert( sq_type( var->obj.weakref->_obj ) != OT_NULL );
	}
	else
	{
		var->obj.obj = obj;
	}

	if ( HasNonStringMembers( obj ) )
		var->obj.hasNonStringMembers = 1;

	return var->id;
}

varref_t *SQDebugServer::FromVarRef( int id )
{
	int hi = m_Vars.size() - 1;
	int lo = 0;

	while ( lo <= hi )
	{
		int mid = lo + ( ( hi - lo ) >> 1 );

		varref_t *var = &m_Vars[mid];

		if ( id > var->id )
		{
			lo = mid + 1;
		}
		else if ( id < var->id )
		{
			hi = mid - 1;
		}
		else
		{
			Assert( var->type >= 0 && var->type < VARREF_MAX );

			if ( IsScopeRef( var->type ) ||
					( !var->obj.isWeakref || sq_type( var->GetVar() ) != OT_NULL ) )
				return var;

			Assert( var->obj.isWeakref );
			Assert( var->obj.weakref );

			__ObjRelease( var->obj.weakref );
			m_Vars.remove(mid);

			return NULL;
		}
	}

	return NULL;
}

void SQDebugServer::RemoveVarRefs( bool all )
{
	if ( !all )
	{
		for ( int i = m_Vars.size(); i--; )
		{
			varref_t &v = m_Vars[i];

			// Keep living weakrefs, client might refer to them later
			if ( IsScopeRef( v.type ) || !v.obj.isWeakref )
			{
				m_Vars.remove(i);
			}
			else
			{
				Assert( v.obj.isWeakref );
				Assert( v.obj.weakref );

				if ( sq_type( v.obj.weakref->_obj ) == OT_NULL )
				{
					__ObjRelease( v.obj.weakref );
					m_Vars.remove(i);
				}
			}
		}
	}
	else
	{
		for ( int i = m_Vars.size(); i--; )
		{
			varref_t &v = m_Vars[i];

			// Release all refs the debugger is holding
			if ( IsObjectRef( v.type ) && v.obj.isWeakref )
			{
				Assert( v.obj.weakref );
				__ObjRelease( v.obj.weakref );
			}
		}

		m_Vars.resize(0);
	}
}

void SQDebugServer::RemoveLockedWatches()
{
	for ( unsigned int i = 0; i < m_LockedWatches.size(); i++ )
		m_LockedWatches[i].expression.Free();

	m_LockedWatches.resize(0);
}

bool SQDebugServer::AddBreakpoint( int line, const string_t &src,
		const string_t &condition, int hitsTarget, const string_t &logMessage, int id )
{
	Assert( line > 0 && !src.IsEmpty() );

	int i;

#ifdef SQUNICODE
	Assert( src.IsTerminated() );
	Assert( UnicodeLength( src.ptr ) < 256 );

	SQChar pSrc[256];
	sqstring_t wsrc( pSrc, UTF8ToUnicode( pSrc, src.ptr, sizeof(pSrc) ) );

	if ( GetBreakpoint( line, wsrc, &i ) )
		return true;
#else
	if ( GetBreakpoint( line, src, &i ) )
		return true;
#endif

	SQObjectPtr condFn;

	if ( !condition.IsEmpty() )
	{
		if ( !CompileScript( condition, condFn ) || sq_type(condFn) == OT_NULL )
			return false;
	}

	breakpoint_t &bp = m_Breakpoints.push_back();
	memset( &bp, 0, sizeof(breakpoint_t) );
	bp.conditionFn._type = OT_NULL;
	bp.conditionEnv._type = OT_NULL;

	bp.id = id;
	bp.src.Copy( src );

	bp.line = line;
	bp.hitsTarget = hitsTarget;

	if ( !logMessage.IsEmpty() )
		bp.logMessage.Copy( logMessage );

	if ( sq_type(condFn) != OT_NULL )
	{
		bp.conditionFn = condFn;
		InitEnv_GetVal( bp.conditionEnv );

		sq_addref( m_pRootVM, &bp.conditionFn );
		sq_addref( m_pRootVM, &bp.conditionEnv );
	}

	return true;
}

bool SQDebugServer::AddFunctionBreakpoint( const string_t &func, const string_t &funcsrc,
		const string_t &condition, int hitsTarget, const string_t &logMessage, int id )
{
	int i;

#ifdef SQUNICODE
	Assert( func.IsTerminated() );
	Assert( UnicodeLength( func.ptr ) < 128 );
	Assert( funcsrc.IsEmpty() || funcsrc.IsTerminated() );
	Assert( funcsrc.IsEmpty() || UnicodeLength( funcsrc.ptr ) < 128 );

	SQChar pFunc[128], pSrc[128];
	sqstring_t wfunc( pFunc, UTF8ToUnicode( pFunc, func.ptr, sizeof(pFunc) ) );
	sqstring_t wsrc( (SQChar*)0, 0 );

	if ( !funcsrc.IsEmpty() )
		wsrc.Assign( pSrc, UTF8ToUnicode( pSrc, funcsrc.ptr, sizeof(pSrc) ) );

	if ( GetFunctionBreakpoint( wfunc, wsrc, &i ) )
		return true;
#else
	if ( GetFunctionBreakpoint( func, funcsrc, &i ) )
		return true;
#endif

	SQObjectPtr condFn;

	if ( !condition.IsEmpty() )
	{
		if ( !CompileScript( condition, condFn ) || sq_type(condFn) == OT_NULL )
			return false;
	}

	breakpoint_t &bp = m_Breakpoints.push_back();
	memset( &bp, 0, sizeof(breakpoint_t) );
	bp.conditionFn._type = OT_NULL;
	bp.conditionEnv._type = OT_NULL;

	bp.id = id;

	if ( !func.IsEmpty() )
		bp.src.Copy( func );

	if ( !funcsrc.IsEmpty() )
		bp.funcsrc.Copy( funcsrc );

	bp.hitsTarget = hitsTarget;

	if ( !logMessage.IsEmpty() )
		bp.logMessage.Copy( logMessage );

	if ( sq_type(condFn) != OT_NULL )
	{
		bp.conditionFn = condFn;
		InitEnv_GetVal( bp.conditionEnv );

		sq_addref( m_pRootVM, &bp.conditionFn );
		sq_addref( m_pRootVM, &bp.conditionEnv );
	}

	return true;
}

breakpoint_t *SQDebugServer::GetBreakpoint( int line, const sqstring_t &src, int *id )
{
	Assert( line && src.ptr );

	for ( unsigned int i = 0; i < m_Breakpoints.size(); i++ )
	{
		breakpoint_t &bp = m_Breakpoints[i];
		if ( bp.line == line && bp.src.IsEqualTo( src ) )
		{
			*id = bp.id;
			return &bp;
		}
	}

	*id = -1;
	return NULL;
}

breakpoint_t *SQDebugServer::GetFunctionBreakpoint( const sqstring_t &func, const sqstring_t &funcsrc, int *id )
{
	for ( unsigned int i = 0; i < m_Breakpoints.size(); i++ )
	{
		breakpoint_t &bp = m_Breakpoints[i];
		if ( bp.line == 0 && bp.src.IsEqualTo( func ) &&
				( !bp.funcsrc.len || bp.funcsrc.IsEqualTo( funcsrc ) ) )
		{
			*id = bp.id;
			return &bp;
		}
	}

	*id = -1;
	return NULL;
}

void SQDebugServer::FreeBreakpoint( breakpoint_t &bp )
{
	if ( bp.src.len )
		bp.src.Free();

	if ( bp.funcsrc.len )
		bp.funcsrc.Free();

	if ( sq_type(bp.conditionFn) != OT_NULL )
	{
		sq_release( m_pRootVM, &bp.conditionFn );
		bp.conditionFn.Null();
	}

	if ( sq_type(bp.conditionEnv) != OT_NULL )
	{
		ClearEnvDelegate( bp.conditionEnv );

		sq_release( m_pRootVM, &bp.conditionEnv );
		bp.conditionEnv.Null();
	}

	bp.logMessage.Free();
	bp.hits = bp.hitsTarget = 0;
}

void SQDebugServer::RemoveAllBreakpoints()
{
	for ( int i = m_Breakpoints.size(); i--; )
		FreeBreakpoint( m_Breakpoints[i] );

	m_Breakpoints.resize(0);
}

void SQDebugServer::RemoveBreakpoints( const string_t &source )
{
#ifdef SQUNICODE
	Assert( source.IsTerminated() );
	Assert( UnicodeLength( source.ptr ) < 128 );

	SQChar tmp[128];
	sqstring_t src( tmp, UTF8ToUnicode( tmp, source.ptr, sizeof(tmp) ) );
#else
	sqstring_t src( source );
#endif

	for ( int i = m_Breakpoints.size(); i--; )
	{
		breakpoint_t &bp = m_Breakpoints[i];
		if ( bp.line != 0 && bp.src.IsEqualTo( src ) )
		{
			FreeBreakpoint( bp );
			m_Breakpoints.remove(i);
		}
	}
}

void SQDebugServer::RemoveFunctionBreakpoints()
{
	for ( int i = m_Breakpoints.size(); i--; )
	{
		breakpoint_t &bp = m_Breakpoints[i];
		if ( bp.line == 0 )
		{
			FreeBreakpoint( bp );
			m_Breakpoints.remove(i);
		}
	}
}

classdef_t *SQDebugServer::FindClassDef( SQClass *base )
{
	for ( unsigned int i = 0; i < m_ClassDefinitions.size(); i++ )
	{
		classdef_t &def = m_ClassDefinitions[i];
		if ( def.base == base )
			return &def;
	}

	return NULL;
}

void SQDebugServer::DefineClass( SQClass *target, SQTable *params )
{
	classdef_t *def = FindClassDef( target );
	if ( !def )
	{
		def = &m_ClassDefinitions.push_back();
		memset( def, 0, sizeof(classdef_t) );
		def->base = target;
		def->value._type = OT_NULL;
		def->metamembers._type = OT_NULL;
	}

	SQObjectPtr name;
	if ( SQTable_Get( params, _SC("name"), name ) && sq_type(name) == OT_STRING && _string(name)->_len )
	{
		stringbuf_t< 1024 > buf;
		buf.PutHex( (SQUnsignedInteger)target );
		buf.Put(' ');
		buf.Puts( sqstring_t( _string(name)->_val, _string(name)->_len ) );
		buf.Term();

		def->name.FreeAndCopy( buf );
	}

	SQObjectPtr value;
	if ( SQTable_Get( params, _SC("value"), value ) &&
			( sq_type(value) == OT_CLOSURE || sq_type(value) == OT_NATIVECLOSURE ) )
	{
		if ( sq_type(def->value) != OT_NULL )
		{
			sq_release( m_pRootVM, &def->value );
			def->value.Null();
		}

		def->value = value;
		sq_addref( m_pRootVM, &def->value );
	}

	SQObjectPtr metamembers;
	if ( SQTable_Get( params, _SC("metamembers"), metamembers ) && sq_type(metamembers) == OT_ARRAY )
	{
		if ( sq_type(def->metamembers) != OT_NULL )
		{
			sq_release( m_pRootVM, &def->metamembers );
			def->metamembers.Null();
		}

		def->metamembers = metamembers;
		sq_addref( m_pRootVM, &def->metamembers );
	}
}

void SQDebugServer::RemoveClassDefs()
{
	for ( unsigned int i = 0; i < m_ClassDefinitions.size(); i++ )
	{
		classdef_t &def = m_ClassDefinitions[i];

		def.name.Free();

		if ( sq_type(def.value) != OT_NULL )
		{
			sq_release( m_pRootVM, &def.value );
			def.value.Null();
		}

		if ( sq_type(def.metamembers) != OT_NULL )
		{
			sq_release( m_pRootVM, &def.metamembers );
			def.metamembers.Null();
		}
	}

	m_ClassDefinitions.resize(0);
}

sqstring_t SQDebugServer::PrintDisassembly( SQClosure *target )
{
	SQFunctionProto *func = _fp(target->_function);

	const int bufsize =
		STRLEN("stacksize     \n") + FMT_INT_LEN +
		STRLEN("instructions  \n") + FMT_INT_LEN +
		STRLEN("literals      \n") + FMT_INT_LEN +
		STRLEN("localvarinfos \n") + FMT_INT_LEN +
		6 + 1 +
		func->_ninstructions * ( 6 + 30 + 128 + 1 ) +
		1;

	SQChar *scratch = _ss(m_pRootVM)->GetScratchPad( bufsize );
	SQChar *buf = scratch;

#define bs (bufsize - (int)((char*)buf - (char*)scratch))

	int instrCount = func->_ninstructions;
	for ( int i = instrCount; i--; )
		if ( func->_instructions[i].op == _OP_LINE )
			instrCount--;

	buf += scsprintf( buf, bs, _SC("stacksize     " FMT_INT "\n"), func->_stacksize );
	buf += scsprintf( buf, bs, _SC("instructions  %d\n"), instrCount );
	buf += scsprintf( buf, bs, _SC("literals      " FMT_INT "\n"), func->_nliterals );
	buf += scsprintf( buf, bs, _SC("localvarinfos " FMT_INT "\n"), func->_nlocalvarinfos );
	for ( int i = 6; i--; ) *buf++ = _SC('-');
	*buf++ = _SC('\n');

	for ( int i = 0, index = 0; i < func->_ninstructions; i++ )
	{
		SQInstruction *instr = func->_instructions + i;
		if ( instr->op == _OP_LINE )
			continue;

		stringbuf_t< 128 > tmp;
		tmp.PutHex( instr->op ); tmp.Put(' ');
		tmp.PutInt( instr->_arg0 ); tmp.Put(' ');
		tmp.PutInt( instr->_arg1 ); tmp.Put(' ');
		tmp.PutInt( instr->_arg2 ); tmp.Put(' ');
		tmp.PutInt( instr->_arg3 );
		tmp.Term();

#ifdef SQUNICODE
		buf += scsprintf( buf, bs, _SC("%-6d %-29hs"), index++, tmp.ptr );
		int len = DescribeInstruction( instr, func, tmp.ptr, sizeof(tmp.ptr) );
		tmp.ptr[len] = 0;
		buf += UTF8ToUnicode( buf, tmp.ptr, bs );
#else
		buf += scsprintf( buf, bs, _SC("%-6d %-29s"), index++, tmp.ptr );
		buf += DescribeInstruction( instr, func, buf, bs );
#endif
		*buf++ = _SC('\n');
	}

	*buf-- = 0;

#undef bs

	return { scratch, (int)(buf - scratch) };
}

#ifndef CALL_DEFAULT_ERROR_HANDLER
void SQDebugServer::PrintVar( HSQUIRRELVM vm, const SQChar* name, const SQObjectPtr &obj )
{
	switch ( sq_type(obj) )
	{
		case OT_NULL:
			SQErrorAtFrame( vm, NULL, _SC("[%s] NULL\n"), name );
			break;
		case OT_INTEGER:
			SQErrorAtFrame( vm, NULL, _SC("[%s] " FMT_INT "\n"), name, _integer(obj) );
			break;
		case OT_FLOAT:
			SQErrorAtFrame( vm, NULL, _SC("[%s] %.14g\n"), name, _float(obj) );
			break;
		case OT_USERPOINTER:
			SQErrorAtFrame( vm, NULL, _SC("[%s] USERPOINTER\n"), name );
			break;
		case OT_STRING:
			SQErrorAtFrame( vm, NULL, _SC("[%s] \"%s\"\n"), name, _stringval(obj) );
			break;
		case OT_TABLE:
			SQErrorAtFrame( vm, NULL, _SC("[%s] TABLE\n"), name );
			break;
		case OT_ARRAY:
			SQErrorAtFrame( vm, NULL, _SC("[%s] ARRAY\n"), name );
			break;
		case OT_CLOSURE:
			SQErrorAtFrame( vm, NULL, _SC("[%s] CLOSURE\n"), name );
			break;
		case OT_NATIVECLOSURE:
			SQErrorAtFrame( vm, NULL, _SC("[%s] NATIVECLOSURE\n"), name );
			break;
		case OT_GENERATOR:
			SQErrorAtFrame( vm, NULL, _SC("[%s] GENERATOR\n"), name );
			break;
		case OT_USERDATA:
			SQErrorAtFrame( vm, NULL, _SC("[%s] USERDATA\n"), name );
			break;
		case OT_THREAD:
			SQErrorAtFrame( vm, NULL, _SC("[%s] THREAD\n"), name );
			break;
		case OT_CLASS:
			SQErrorAtFrame( vm, NULL, _SC("[%s] CLASS\n"), name );
			break;
		case OT_INSTANCE:
			SQErrorAtFrame( vm, NULL, _SC("[%s] INSTANCE\n"), name );
			break;
		case OT_WEAKREF:
			PrintVar( vm, name, _weakref(obj)->_obj );
			break;
		case OT_BOOL:
			SQErrorAtFrame( vm, NULL, _SC("[%s] %s\n"), name, _integer(obj) ? _SC("true") : _SC("false") );
			break;
		default: UNREACHABLE();
	}
}

void SQDebugServer::PrintStack( HSQUIRRELVM vm )
{
	SQErrorAtFrame( vm, NULL, _SC("\nCALLSTACK\n") );

	int i = vm->_callsstacksize;
	while ( i-- )
	{
		const SQVM::CallInfo &ci = vm->_callsstack[i];

		if ( ShouldIgnoreStackFrame(ci) )
			continue;

		if ( sq_type(ci._closure) == OT_CLOSURE )
		{
			SQFunctionProto *func = _fp(_closure(ci._closure)->_function);

			sqstring_t src( _SC("unknown") );
			int line = func->GetLine( ci._ip );
			const SQChar *fn = _SC("unknown");;

			if ( sq_type(func->_name) == OT_STRING )
				fn = _stringval(func->_name);

			if ( sq_type(func->_sourcename) == OT_STRING )
				src.Assign( _string(func->_sourcename) );

			SQErrorAtFrame( vm, &ci, _SC("*FUNCTION [%s()] %s line [%d]\n"), fn, src.ptr, line );
		}
		else if ( sq_type(ci._closure) == OT_NATIVECLOSURE )
		{
			SQNativeClosure *closure = _nativeclosure(ci._closure);

			const SQChar *src = _SC("NATIVE");
			int line = -1;
			const SQChar *fn = _SC("unknown");

			if ( sq_type(closure->_name) == OT_STRING )
				fn = _stringval(closure->_name);

			SQErrorAtFrame( vm, NULL, _SC("*FUNCTION [%s()] %s line [%d]\n"), fn, src, line );
		}
		else UNREACHABLE();
	}

	SQErrorAtFrame( vm, NULL, _SC("\nLOCALS\n") );

	i = vm->_callsstacksize;
	if ( i > 10 )
		i = 10;

	while ( i-- )
	{
		const SQVM::CallInfo &ci = vm->_callsstack[i];

		if ( sq_type(ci._closure) != OT_CLOSURE )
			continue;

		if ( ShouldIgnoreStackFrame(ci) )
			continue;

		int stackbase = GetStackBase( vm, i );
		SQClosure *pClosure = _closure(ci._closure);
		SQFunctionProto *func = _fp(pClosure->_function);

		SQUnsignedInteger ip = (SQUnsignedInteger)( ci._ip - func->_instructions ) - 1;

		for ( int i = 0; i < func->_nlocalvarinfos; i++ )
		{
			const SQLocalVarInfo &var = func->_localvarinfos[i];
			if ( var._start_op <= ip && var._end_op >= ip )
			{
				PrintVar( vm, _stringval(var._name), vm->_stack._vals[ stackbase + var._pos ] );
			}
		}

		for ( int i = 0; i < func->_noutervalues; i++ )
		{
			const SQOuterVar &var = func->_outervalues[i];
			PrintVar( vm, _stringval(var._name), *_outervalptr( pClosure->_outervalues[i] ) );
		}
	}
}
#endif

void SQDebugServer::ErrorHandler( HSQUIRRELVM vm )
{
	if ( m_bIgnoreExceptions && !m_bIgnoreDebugHookGuard )
		return;

	string_t err;

	if ( sq_gettop( vm ) >= 1 )
	{
		HSQOBJECT o;
		sq_getstackobj( vm, 2, &o );
		err = GetValue( o, NO_QUOT );
	}
	else
	{
		err.Assign( "unknown" );
	}

	// An error handler is required to detect exceptions.
	// The downside of calling the default error handler instead of
	// replicating it in the debugger is the extra stack frame and redundant print locations.
	// Otherwise this would be preferrable for preserving custom error handlers.
#ifdef CALL_DEFAULT_ERROR_HANDLER
	SQObjectPtr dummy;
	vm->Call( m_ErrorHandler, 2, vm->_top-2, dummy, SQFalse );
#else
	SQErrorAtFrame( vm, NULL, _SC("\nAN ERROR HAS OCCURRED [" FMT_VCSTR "]\n"), STR_EXPAND(err) );
	PrintStack( vm );
#endif

	if ( m_bBreakOnExceptions && IsClientConnected() )
	{
		m_BreakReason.reason = BreakReason::Exception;
		m_BreakReason.text.Assign( err );
		Break( vm );

#if SQUIRREL_VERSION_NUMBER < 300
		// SQ2 doesn't notify the debug hook of returns via errors, have to suspend here.
		// Use m_bExceptionPause to make stepping while suspended here continue the thread.
		if ( m_State == ThreadState::SuspendNow )
		{
			m_bExceptionPause = true;
			m_bInDebugHook = true;
			Suspend();
			m_bExceptionPause = false;
			m_bInDebugHook = false;
		}
#endif
	}
}

void SQDebugServer::Break( HSQUIRRELVM vm )
{
	Assert( m_BreakReason.reason != BreakReason::None );

	DAP_START_EVENT( m_Sequence++, "stopped" );
	DAP_SET_TABLE( body, 5 );
		body.SetInt( "threadId", ThreadToID( vm ) );
		body.SetBool( "allThreadsStopped", true );

		switch ( m_BreakReason.reason )
		{
			case BreakReason::Step:
				body.SetString( "reason", "step" );
				break;
			case BreakReason::Breakpoint:
				body.SetString( "reason", "breakpoint" );
				break;
			case BreakReason::Exception:
				body.SetString( "reason", "exception" );
				break;
			case BreakReason::Pause:
				body.SetString( "reason", "pause" );
				break;
			case BreakReason::Restart:
				body.SetString( "reason", "restart" );
				break;
			case BreakReason::Goto:
				body.SetString( "reason", "goto" );
				break;
			case BreakReason::FunctionBreakpoint:
				body.SetString( "reason", "function breakpoint" );
				break;
			case BreakReason::DataBreakpoint:
				body.SetString( "reason", "data breakpoint" );
				break;
			default: UNREACHABLE();
		}

		if ( !m_BreakReason.text.IsEmpty() )
			body.SetStringNoCopy( "text", m_BreakReason.text );

		if ( m_BreakReason.reason == BreakReason::Breakpoint ||
				m_BreakReason.reason == BreakReason::FunctionBreakpoint ||
				m_BreakReason.reason == BreakReason::DataBreakpoint )
		{
			body.SetArray( "hitBreakpointIds", 1 ).Append( m_BreakReason.id );
		}
	DAP_SEND();

	if ( m_State != ThreadState::Suspended )
		m_State = ThreadState::SuspendNow;

	m_BreakReason.reason = BreakReason::None;
	m_BreakReason.text.ptr = NULL;
	m_BreakReason.id = 0;
}

void SQDebugServer::Suspend()
{
	Assert( m_State == ThreadState::SuspendNow );

	m_State = ThreadState::Suspended;

	do
	{
		if ( IsClientConnected() )
		{
			Frame();
		}
		else
		{
			Continue( NULL );
			break;
		}

		Sleep( 20 );
	}
	while ( m_State == ThreadState::Suspended );
}

void SQDebugServer::Continue( HSQUIRRELVM vm )
{
	if ( m_State == ThreadState::SuspendNow )
		return;

	if ( IsClientConnected() && m_State != ThreadState::Running )
	{
		DAP_START_EVENT( m_Sequence++, "continued" );
		DAP_SET_TABLE( body, 2 );
			body.SetInt( "threadId", ThreadToID( vm ) );
			body.SetBool( "allThreadsContinued", true );
		DAP_SEND();
	}

	m_State = ThreadState::Running;
	m_pPausedThread = NULL;
	m_ReturnValue.Null();
	RemoveVarRefs( false );
	RemoveLockedWatches();
}

//
// Expressions within `{}` are evaluated.
// Escape the opening bracket to print brackets `\{`
//
// Special keywords: $FUNCTION, $CALLER, $HITCOUNT
//
void SQDebugServer::TracePoint( breakpoint_t *bp, HSQUIRRELVM vm, int frame )
{
	char buf[512];
	int bufsize = sizeof(buf) - 2; // \n\0
	int readlen = min( bufsize, bp->logMessage.len );
	char *pWrite = buf;
	const char *logMessage = bp->logMessage.ptr;

	for ( int iRead = 0; iRead < readlen; iRead++ )
	{
		switch ( logMessage[iRead] )
		{
			case '{':
			{
				// '\' preceeds '{'
				if ( iRead && logMessage[iRead-1] == '\\' )
				{
					pWrite[-1] = '{';
					continue;
				}

				int depth = 1;
				for ( int j = iRead + 1; j < readlen; j++ )
				{
					switch ( logMessage[j] )
					{
						case '}':
						{
							depth--;

							// Found expression
							if ( depth == 0 )
							{
								string_t expression( logMessage + iRead + 1, j - iRead - 1 );

								if ( expression.len )
								{
									pWrite += EvalAndWriteExpr( pWrite, bufsize - j, expression, vm, frame );
								}

								iRead = j;
								goto exit;
							}

							break;
						}
						case '{':
						{
							depth++;
							break;
						}
					}
				}
			exit:;
				 break;
			}
			case '$':
			{
				#define STRCMP( s, StrLiteral ) \
					memcmp( (s), (StrLiteral), sizeof(StrLiteral)-1 )

				#define CHECK_KEYWORD(s) \
					( ( iRead + (int)STRLEN(s) < readlen ) && \
						!STRCMP( logMessage + iRead + 1, s ) )

				if ( CHECK_KEYWORD("FUNCTION") )
				{
					const SQObjectPtr &funcname = _fp(_closure(vm->_callsstack[frame]._closure)->_function)->_name;

					if ( sq_type(funcname) == OT_STRING )
					{
						sqstring_t name( _string(funcname) );
						int writelen = scstombs( pWrite, bufsize - iRead, name.ptr, name.len );
						pWrite += writelen;
					}

					iRead += STRLEN("FUNCTION");
					break;
				}
				else if ( CHECK_KEYWORD("CALLER") )
				{
					if ( frame > 0 )
					{
						sqstring_t name;
						const SQVM::CallInfo &ci = vm->_callsstack[frame-1];

						if ( sq_type(ci._closure) == OT_CLOSURE )
						{
							if ( sq_type(_fp(_closure(ci._closure)->_function)->_name) == OT_STRING )
							{
								name.Assign( _string(_fp(_closure(ci._closure)->_function)->_name) );
							}
							else
							{
								name.Assign( _SC(""), 0 );
							}
						}
						else if ( sq_type(ci._closure) == OT_NATIVECLOSURE )
						{
							if ( sq_type(_nativeclosure(ci._closure)->_name) == OT_STRING )
							{
								name.Assign( _string(_nativeclosure(ci._closure)->_name) );
							}
							else
							{
								name.Assign( _SC(""), 0 );
							}
						}

						if ( name.len )
						{
							int writelen = scstombs( pWrite, bufsize - iRead, name.ptr, name.len );
							pWrite += writelen;
						}
					}

					iRead += STRLEN("CALLER");
					break;
				}
				else if ( CHECK_KEYWORD("HITCOUNT") )
				{
					// lazy hack, hit count was reset after hitting the target
					// if this count is to ignore hit target, keep trace hit count separately
					int hits = bp->hits ? bp->hits : bp->hitsTarget;
					pWrite += printint( pWrite, bufsize - iRead, hits );
					iRead += STRLEN("HITCOUNT");
					break;
				}
				// else fallthrough
			}
			default:
				*pWrite++ = logMessage[iRead];
		}
	}

	*pWrite++ = '\n';
	*pWrite = 0;

#ifdef SQUNICODE
	m_Print( vm, _SC(FMT_CSTR), buf );
#else
	m_Print( vm, buf );
#endif

#ifdef NATIVE_DEBUG_HOOK
	const SQVM::CallInfo *ci = vm->ci;
#else
	const SQVM::CallInfo *ci = vm->ci - 1;
#endif

	SendEvent_OutputStdOut( string_t( buf, (int)( pWrite - buf ) ), ci );
}

bool SQDebugServer::HasCondition( const breakpoint_t *bp )
{
	return ( sq_type( bp->conditionFn ) != OT_NULL );
}

bool SQDebugServer::CheckBreakpointCondition( breakpoint_t *bp, HSQUIRRELVM vm, int frame )
{
	Assert( HasCondition( bp ) );
	Assert( sq_type( bp->conditionFn ) != OT_NULL &&
			sq_type( bp->conditionEnv ) != OT_NULL );

	CacheLocals( vm, frame, bp->conditionEnv );

	SQObjectPtr res;
	if ( RunClosure( bp->conditionFn, &bp->conditionEnv, res ) )
	{
		return !vm->IsFalse( res );
	}
	else
	{
		// Invalid condition, remove it
		ClearEnvDelegate( bp->conditionEnv );

		sq_release( m_pRootVM, &bp->conditionFn );
		sq_release( m_pRootVM, &bp->conditionEnv );

		bp->conditionFn.Null();
		bp->conditionEnv.Null();

		return false;
	}
}


#define SQ_HOOK_LINE _SC('l')
#define SQ_HOOK_CALL _SC('c')
#define SQ_HOOK_RETURN _SC('r')

void SQDebugServer::DebugHook( HSQUIRRELVM vm, SQInteger type,
		const SQChar *sourcename, SQInteger line, const SQChar *funcname )
{
	if ( !IsClientConnected() )
		return;

	if ( m_bDebugHookGuard && !m_bIgnoreDebugHookGuard )
		return;

#if SQUIRREL_VERSION_NUMBER < 300
	// Debug hook re-entry guard doesn't exist in SQ2
	if ( m_bInDebugHook )
		return;

	m_bInDebugHook = true;
#endif

	// The only way to detect thread change, not ideal
	if ( m_pCurVM != vm )
	{
		ThreadToID( vm );
		m_pCurVM = vm;
	}

	if ( m_pPausedThread == vm )
	{
		m_pPausedThread = NULL;
		m_BreakReason.reason = BreakReason::Pause;
		Break( vm );

		if ( m_State == ThreadState::SuspendNow )
		{
			Suspend();
		}

#if SQUIRREL_VERSION_NUMBER < 300
		m_bInDebugHook = false;
#endif
		return;
	}

	if ( m_DataWatches.size() )
	{
		CheckDataBreakpoints( vm );
	}

	switch ( m_State )
	{
		case ThreadState::Running:
			break;

		// Continue until next line
		case ThreadState::NextLine:
		{
			if ( m_nCalls == m_nStateCalls )
			{
				switch ( type )
				{
					case SQ_HOOK_LINE:
					{
						m_BreakReason.reason = BreakReason::Step;
						break;
					}
					case SQ_HOOK_RETURN:
					{
						m_nStateCalls--;
						break;
					}
				}
			}

			break;
		}
		// Break at next line
		case ThreadState::StepIn:
		{
			if ( type == SQ_HOOK_LINE )
			{
				m_BreakReason.reason = BreakReason::Step;
			}

			break;
		}
		// Break now
		case ThreadState::NextStatement:
		{
			m_BreakReason.reason = BreakReason::Step;
			break;
		}
		// Break after return
		case ThreadState::StepOut:
		{
			if ( type == SQ_HOOK_RETURN &&
					m_nCalls == m_nStateCalls )
			{
				m_State = ThreadState::NextStatement;
			}

			break;
		}
		default: break;
	}

	switch ( type )
	{
		case SQ_HOOK_LINE:
		{
			Assert( sourcename );

			if ( !sourcename )
				break;

			int srclen = ( (SQString*)( (char*)sourcename - (char*)offsetof( SQString, _val ) ) )->_len;
			Assert( (int)scstrlen(sourcename) == srclen );

#ifdef SOURCENAME_HAS_PATH
			for ( const SQChar *c = sourcename + srclen - 1; c > sourcename; c-- )
			{
				if ( *c == '/' || *c == '\\' )
				{
					c++;
					srclen = sourcename + srclen - c;
					sourcename = c;
					break;
				}
			}
#endif

			sqstring_t src( sourcename, srclen );

			int id;
			breakpoint_t *bp = GetBreakpoint( line, src, &id );
			if ( bp )
			{
#ifdef NATIVE_DEBUG_HOOK
				int curframe = GetCurrentStackFrame( vm );
#else
				int curframe = GetCurrentStackFrame( vm ) - 1;
#endif
				if ( HasCondition( bp ) &&
						!CheckBreakpointCondition( bp, vm, curframe ) )
					break;

				++bp->hits;

				if ( bp->hitsTarget )
				{
					if ( bp->hits < bp->hitsTarget )
						break;

					bp->hits = 0;
				}

				if ( bp->logMessage.IsEmpty() )
				{
					stringbuf_t< 64 > buf;
					buf.Puts( "(sqdbg) Breakpoint hit " );
					buf.Puts( src );
					buf.Put(':');
					buf.PutInt( line );
					buf.Put('\n');
					buf.Term();
#ifdef NATIVE_DEBUG_HOOK
					const SQVM::CallInfo *ci = vm->ci;
#else
					const SQVM::CallInfo *ci = &vm->_callsstack[ curframe ];
#endif
					SendEvent_OutputStdOut( string_t( buf ), ci );
					m_BreakReason.reason = BreakReason::Breakpoint;
					m_BreakReason.id = id;
				}
				else
				{
					TracePoint( bp, vm, curframe );
				}
			}

			break;
		}
		case SQ_HOOK_CALL:
		{
			m_nCalls++;

			sqstring_t func( _SC(""), 0 );
			sqstring_t src( _SC(""), 0 );

			Assert( sourcename );

			if ( funcname )
			{
				int funclen = ( (SQString*)( (char*)funcname - (char*)offsetof( SQString, _val ) ) )->_len;
				Assert( (int)scstrlen(funcname) == funclen );

				func.Assign( funcname, funclen );
			}

			if ( sourcename )
			{
				int srclen = ( (SQString*)( (char*)sourcename - (char*)offsetof( SQString, _val ) ) )->_len;
				Assert( (int)scstrlen(sourcename) == srclen );

#ifdef SOURCENAME_HAS_PATH
				for ( const SQChar *c = sourcename + srclen - 1; c > sourcename; c-- )
				{
					if ( *c == '/' || *c == '\\' )
					{
						c++;
						srclen = sourcename + srclen - c;
						sourcename = c;
						break;
					}
				}
#endif

				src.Assign( sourcename, srclen );
			}

			int id;
			breakpoint_t *bp = GetFunctionBreakpoint( func, src, &id );
			if ( bp )
			{
				if ( HasCondition( bp ) &&
#ifdef NATIVE_DEBUG_HOOK
						!CheckBreakpointCondition( bp, vm, GetCurrentStackFrame( vm ) ) )
#else
						!CheckBreakpointCondition( bp, vm, GetCurrentStackFrame( vm ) - 1 ) )
#endif
					break;

				if ( bp->hitsTarget )
				{
					if ( ++bp->hits < bp->hitsTarget )
						break;

					bp->hits = 0;
				}

				stringbuf_t< 128 > buf;

				if ( funcname )
				{
					if ( bp->funcsrc.IsEmpty() )
					{
						buf.Puts( "(sqdbg) Breakpoint hit " );
						buf.Puts( func );
						buf.Puts( "()\n" );
						buf.Term();
					}
					else
					{
						buf.Puts( "(sqdbg) Breakpoint hit " );
						buf.Puts( func );
						buf.Puts( "() @ " );
						buf.Puts( src );
						buf.Put('\n');
						buf.Term();
					}
				}
				else
				{
					if ( bp->funcsrc.IsEmpty() )
					{
						buf.Puts( "(sqdbg) Breakpoint hit 'anonymous function'\n" );
						buf.Term();
					}
					else
					{
						buf.Puts( "(sqdbg) Breakpoint hit 'anonymous function' @ " );
						buf.Puts( src );
						buf.Put('\n');
						buf.Term();
					}
				}

#ifdef NATIVE_DEBUG_HOOK
				const SQVM::CallInfo *ci = vm->ci;
#else
				const SQVM::CallInfo *ci = vm->ci - 1;
#endif
				SendEvent_OutputStdOut( string_t( buf ), ci );
				m_BreakReason.reason = BreakReason::FunctionBreakpoint;
				m_BreakReason.id = id;
			}

			break;
		}
		case SQ_HOOK_RETURN:
		{
			m_nCalls--;

			if ( m_State != ThreadState::Running &&
					vm == m_pRootVM && // if exiting thread, step into root
#ifdef NATIVE_DEBUG_HOOK
					GetCurrentStackFrame( vm ) == 0 )
#else
					GetCurrentStackFrame( vm ) - 1 == 0 )
#endif
			{
				Continue( vm );
			}
			else if ( m_State == ThreadState::NextLine ||
					m_State == ThreadState::StepIn ||
					// check StepOut, it was changed to this above
					( m_State == ThreadState::NextStatement && m_nCalls+1 == m_nStateCalls ) )
			{
#ifdef NATIVE_DEBUG_HOOK
				const SQVM::CallInfo *ci = vm->ci;
#else
				const SQVM::CallInfo *ci = vm->ci - 1;
#endif

				Assert( sq_type(ci->_closure) == OT_CLOSURE );

				// Is this a generator call?
				bool bGen = ( _fp(_closure(ci->_closure)->_function)->_bgenerator &&
						ci->_ip == _fp(_closure(ci->_closure)->_function)->_instructions );

				if ( !bGen && (ci->_ip-1)->op == _OP_RETURN && (ci->_ip-1)->_arg0 != 0xFF )
				{
#ifdef NATIVE_DEBUG_HOOK
					m_ReturnValue = vm->_stack._vals[ vm->_stackbase + (ci->_ip-1)->_arg1 ];
#else
					m_ReturnValue = vm->_stack._vals[
						GetStackBase( vm, GetCurrentStackFrame( vm ) - 1 ) + (ci->_ip-1)->_arg1 ];
#endif
				}
				else
				{
					m_ReturnValue.Null();
				}
			}

			break;
		}
		default: UNREACHABLE();
	}

	if ( m_BreakReason.reason )
	{
		Break( vm );
	}

	if ( m_State == ThreadState::SuspendNow )
	{
		Suspend();
	}

#if SQUIRREL_VERSION_NUMBER < 300
	m_bInDebugHook = false;
#endif
}

template < typename T >
void SQDebugServer::SendEvent_OutputStdOut( const T &strOutput, const SQVM::CallInfo *ci )
{
	Assert( !ci || sq_type(ci->_closure) == OT_CLOSURE );

	DAP_START_EVENT( m_Sequence++, "output" );
	DAP_SET_TABLE( body, 4 );
		body.SetString( "category", "stdout" );
		body.SetStringNoCopy( "output", strOutput );
		if ( ci )
		{
			SQFunctionProto *func = _fp(_closure(ci->_closure)->_function);
			if ( !sqstring_t(_SC("sqdbg")).IsEqualTo( _string(func->_sourcename) ) )
			{
				body.SetInt( "line", (int)func->GetLine( ci->_ip ) );
				json_table_t &source = body.SetTable( "source", 2 );
				source.SetString( "name", sqstring_t( _string(func->_sourcename) ) );
				sqstring_t *path = m_FilePathMap.Get( _string(func->_sourcename) );
				if ( path )
					source.SetString( "path", *path );
			}
		}
	DAP_SEND();
}

void SQDebugServer::OnSQPrint( HSQUIRRELVM vm, const SQChar *buf, int len )
{
	m_Print( vm, buf );

	if ( IsClientConnected() )
	{
		const SQVM::CallInfo *ci = NULL;

		// Assume the latest script function is the output source
		for ( int i = vm->_callsstacksize; i-- > 0; )
		{
			if ( sq_type(vm->_callsstack[i]._closure) == OT_CLOSURE )
			{
				ci = vm->_callsstack + i;
				break;
			}
		}

		SendEvent_OutputStdOut( sqstring_t( buf, len ), ci );
	}
}

void SQDebugServer::OnSQError( HSQUIRRELVM vm, const SQChar *buf, int len )
{
	m_PrintError( vm, buf );

	if ( IsClientConnected() )
	{
		const SQVM::CallInfo *ci = NULL;

		// Assume the latest script function is the output source
		for ( int i = vm->_callsstacksize; i-- > 0; )
		{
			if ( sq_type(vm->_callsstack[i]._closure) == OT_CLOSURE )
			{
				ci = vm->_callsstack + i;
				break;
			}
		}

		SendEvent_OutputStdOut( sqstring_t( buf, len ), ci );
	}
}


static inline HSQDEBUGSERVER sqdbg_get_debugger( HSQUIRRELVM vm );

SQInteger SQDebugServer::SQDefineClass( HSQUIRRELVM vm )
{
	SQDebugServer *dbg = (SQDebugServer *)sqdbg_get_debugger( vm );
	Assert( dbg );
	if ( dbg )
	{
		HSQOBJECT target;
		HSQOBJECT params;

		sq_getstackobj( vm, -2, &target );
		sq_getstackobj( vm, -1, &params );

		Assert( sq_isclass( target ) && sq_istable( params ) );

		dbg->DefineClass( _class(target), _table(params) );
	}

	return 0;
}

SQInteger SQDebugServer::SQPrintDisassembly( HSQUIRRELVM vm )
{
	SQDebugServer *dbg = (SQDebugServer *)sqdbg_get_debugger( vm );
	Assert( dbg );
	if ( dbg )
	{
		HSQOBJECT target;
		sq_getstackobj( vm, -1, &target );

		if ( !sq_isclosure( target ) )
			return sq_throwerror( vm, _SC("the object is not a closure") );

		sqstring_t str = dbg->PrintDisassembly( _closure(target) );
		sq_pushstring( vm, str.ptr, str.len );
		return 1;
	}
	else
	{
		sq_pushnull( vm );
		return 1;
	}
}

SQInteger SQDebugServer::SQBreak( HSQUIRRELVM vm )
{
	SQDebugServer *dbg = (SQDebugServer *)sqdbg_get_debugger( vm );
	Assert( dbg );
	if ( dbg )
	{
		if ( dbg->m_State == ThreadState::Running &&
				!( dbg->m_bDebugHookGuard && !dbg->m_bIgnoreDebugHookGuard ) )
		{
			dbg->m_pPausedThread = vm;
		}
	}

	return 0;
}

#define SQDBG_PRINTBUF_SIZE 2048

void SQDebugServer::SQPrint( HSQUIRRELVM vm, const SQChar *fmt, ... )
{
	SQDebugServer *dbg = (SQDebugServer *)sqdbg_get_debugger( vm );
	Assert( dbg );
	if ( dbg )
	{
		SQChar buf[ SQDBG_PRINTBUF_SIZE ];
		va_list va;
		va_start( va, fmt );
		int len = scvsprintf( buf, SQDBG_PRINTBUF_SIZE, fmt, va );
		va_end( va );

		if ( len < 0 || len > SQDBG_PRINTBUF_SIZE-1 )
			len = SQDBG_PRINTBUF_SIZE-1;

		dbg->OnSQPrint( vm, buf, len );
	}
}

void SQDebugServer::SQError( HSQUIRRELVM vm, const SQChar *fmt, ... )
{
	SQDebugServer *dbg = (SQDebugServer *)sqdbg_get_debugger( vm );
	Assert( dbg );
	if ( dbg )
	{
		SQChar buf[ SQDBG_PRINTBUF_SIZE ];
		va_list va;
		va_start( va, fmt );
		int len = scvsprintf( buf, SQDBG_PRINTBUF_SIZE, fmt, va );
		va_end( va );

		if ( len < 0 || len > SQDBG_PRINTBUF_SIZE-1 )
			len = SQDBG_PRINTBUF_SIZE-1;

		dbg->OnSQError( vm, buf, len );
	}
}

#ifndef CALL_DEFAULT_ERROR_HANDLER
void SQDebugServer::SQErrorAtFrame( HSQUIRRELVM vm, const SQVM::CallInfo *ci, const SQChar *fmt, ... )
{
	SQDebugServer *dbg = (SQDebugServer *)sqdbg_get_debugger( vm );
	Assert( dbg );
	if ( dbg )
	{
		SQChar buf[ SQDBG_PRINTBUF_SIZE ];
		va_list va;
		va_start( va, fmt );
		int len = scvsprintf( buf, SQDBG_PRINTBUF_SIZE, fmt, va );
		va_end( va );

		if ( len < 0 || len > SQDBG_PRINTBUF_SIZE-1 )
			len = SQDBG_PRINTBUF_SIZE-1;

		dbg->m_PrintError( vm, buf );
		dbg->SendEvent_OutputStdOut( sqstring_t( buf, len ), ci );
	}
}
#endif

SQInteger SQDebugServer::SQErrorHandler( HSQUIRRELVM vm )
{
	SQDebugServer *dbg = (SQDebugServer *)sqdbg_get_debugger( vm );
	Assert( dbg );
	if ( dbg )
	{
		dbg->ErrorHandler( vm );
	}

	return 0;
}

#ifdef NATIVE_DEBUG_HOOK
void SQDebugServer::SQDebugHook( HSQUIRRELVM vm, SQInteger type,
		const SQChar *sourcename, SQInteger line, const SQChar *funcname )
{
	SQDebugServer *dbg = (SQDebugServer *)sqdbg_get_debugger( vm );
	Assert( dbg );
	if ( dbg )
	{
		dbg->DebugHook( vm, type, sourcename, line, funcname );
	}
}
#else
SQInteger SQDebugServer::SQDebugHook( HSQUIRRELVM vm )
{
	SQDebugServer *dbg = (SQDebugServer *)sqdbg_get_debugger( vm );
	Assert( dbg );
	if ( dbg )
	{
		HSQOBJECT type;
		HSQOBJECT sourcename;
		HSQOBJECT line;
		HSQOBJECT funcname;

		sq_getstackobj( vm, -4, &type );
		sq_getstackobj( vm, -3, &sourcename );
		sq_getstackobj( vm, -2, &line );
		sq_getstackobj( vm, -1, &funcname );

		Assert( sq_isinteger( type ) &&
				( sq_isstring( sourcename ) || sq_isnull( sourcename ) ) &&
				sq_isinteger( line ) &&
				( sq_isstring( funcname ) || sq_isnull( funcname ) ) );

		const SQChar *src = sq_isstring( sourcename ) ? _stringval( sourcename ) : NULL;
		const SQChar *fun = sq_isstring( funcname ) ? _stringval( funcname ) : NULL;

		dbg->DebugHook( vm, _integer(type), src, _integer(line), fun );
	}

	return 0;
}
#endif

SQInteger SQDebugServer::SQRelease( SQUserPointer pDebugServer, SQInteger size )
{
	(void)size;
	SQDebugServer *dbg = (SQDebugServer*)pDebugServer;

	dbg->Shutdown();
	dbg->~SQDebugServer();

	return 0;
}

HSQDEBUGSERVER sqdbg_get_debugger( HSQUIRRELVM vm )
{
	// Use SQTable_Get to reduce hot path with stack operations
	SQObjectPtr dbg;
	if ( SQTable_Get( _table(_ss(vm)->_registry), _SC(SQDBG_SV_TAG), dbg ) )
	{
		Assert( sq_isuserdata( dbg ) );
		return (HSQDEBUGSERVER)_userdataval(dbg);
	}

	return NULL;
}

HSQDEBUGSERVER sqdbg_attach_debugger( HSQUIRRELVM vm )
{
	CStackCheck stackcheck( vm );

	sq_pushregistrytable( vm );
	sq_pushstring( vm, _SC(SQDBG_SV_TAG), -1 );

	if ( SQ_SUCCEEDED( sq_get( vm, -2 ) ) )
	{
		HSQDEBUGSERVER dbg;
		sq_getuserdata( vm, -1, (SQUserPointer*)&dbg, NULL );
		sq_pop( vm, 2 );
		return dbg;
	}
	else
	{
		sq_pushstring( vm, _SC(SQDBG_SV_TAG), -1 );
		SQDebugServer *dbg = (SQDebugServer*)sq_newuserdata( vm, sizeof(SQDebugServer) );
		new (dbg) SQDebugServer;
		sq_setreleasehook( vm, -1, &SQDebugServer::SQRelease );
		sq_newslot( vm, -3, SQFalse );
		sq_pop( vm, 1 );

		dbg->Attach( vm );

		return dbg;
	}
}

void sqdbg_destroy_debugger( HSQUIRRELVM vm )
{
#ifdef _DEBUG
	{
		CStackCheck stackcheck( vm );
		sq_pushregistrytable( vm );
		sq_pushstring( vm, _SC(SQDBG_SV_TAG), -1 );
		if ( SQ_SUCCEEDED( sq_get( vm, -2 ) ) )
		{
			HSQOBJECT dbg;
			sq_getstackobj( vm, -1, &dbg );
			// refs: stack + registry
			// it will be released at the end of this
			Assert( sq_isuserdata( dbg ) && _refcounted(dbg)->_uiRef == 2 );
			sq_pop( vm, 2 );
		}
		else
		{
			sq_pop( vm, 1 );
		}
	}
#endif

	CStackCheck stackcheck( vm );
	sq_pushregistrytable( vm );
	sq_pushstring( vm, _SC(SQDBG_SV_TAG), -1 );
	sq_deleteslot( vm, -2, SQFalse );
	sq_pop( vm, 1 );
}

int sqdbg_listen_socket( HSQDEBUGSERVER dbg, unsigned short port )
{
	return ( dbg->ListenSocket( port ) == false );
}

void sqdbg_frame( HSQDEBUGSERVER dbg )
{
	dbg->Frame();
}

SQRESULT sqdbg_compilebuffer( HSQUIRRELVM vm, const SQChar *script, SQInteger size,
		const SQChar *sourcename, SQBool raiseerror )
{
	SQRESULT ret = sq_compilebuffer( vm, script, size, sourcename, raiseerror );

#if SQUIRREL_VERSION_NUMBER >= 300
	if ( !vm->_debughook )
#else
	if ( sq_type(vm->_debughook) != OT_NULL )
#endif
		return ret;

	if ( SQ_SUCCEEDED(ret) )
	{
		SQDebugServer *dbg = (SQDebugServer *)sqdbg_get_debugger( vm );
		Assert( dbg );
		if ( dbg )
		{
			dbg->OnScriptCompile( script, size, sourcename );
		}
	}

	return ret;
}
