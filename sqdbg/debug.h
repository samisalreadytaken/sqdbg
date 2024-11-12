//-----------------------------------------------------------------------
//                       github.com/samisalreadytaken/sqdbg
//-----------------------------------------------------------------------
//

#ifndef SQDBG_DEBUG_H
#define SQDBG_DEBUG_H

#ifdef _DEBUG
	#ifdef _WIN32
		#include <crtdbg.h>

		extern "C" WINBASEAPI BOOL WINAPI IsDebuggerPresent( VOID );

		static inline const char *GetModuleBaseName()
		{
			static char module[MAX_PATH];
			DWORD len = GetModuleFileNameA( NULL, module, sizeof(module) );

			if ( len != 0 )
			{
				for ( char *pBase = module + len; pBase-- > module; )
				{
					if ( *pBase == '\\' )
						return pBase + 1;
				}

				return module;
			}

			return "";
		}

		#define DebuggerBreak() do { if ( IsDebuggerPresent() ) __debugbreak(); } while(0)

		#define Assert( x ) \
			do { \
				__CAT( L, __LINE__ ): \
				if ( !(x) && (1 == _CrtDbgReport(_CRT_ASSERT, __FILE__, __LINE__, GetModuleBaseName(), #x)) ) \
				{ \
					if ( !IsDebuggerPresent() ) \
						goto __CAT( L, __LINE__ ); \
					__debugbreak(); \
				} \
			} while(0)

		#define AssertMsg( x, msg ) \
			do { \
				__CAT( L, __LINE__ ): \
				if ( !(x) && (1 == _CrtDbgReport(_CRT_ASSERT, __FILE__, __LINE__, GetModuleBaseName(), msg)) ) \
				{ \
					if ( !IsDebuggerPresent() ) \
						goto __CAT( L, __LINE__ ); \
					__debugbreak(); \
				} \
			} while(0)

		#define AssertMsg1( x, msg, a1 ) \
			do { \
				__CAT( L, __LINE__ ): \
				if ( !(x) && (1 == _CrtDbgReport(_CRT_ASSERT, __FILE__, __LINE__, GetModuleBaseName(), msg, a1)) ) \
				{ \
					if ( !IsDebuggerPresent() ) \
						goto __CAT( L, __LINE__ ); \
					__debugbreak(); \
				} \
			} while(0)

		#define AssertMsg2( x, msg, a1, a2 ) \
			do { \
				__CAT( L, __LINE__ ): \
				if ( !(x) && (1 == _CrtDbgReport(_CRT_ASSERT, __FILE__, __LINE__, GetModuleBaseName(), msg, a1, a2)) ) \
				{ \
					if ( !IsDebuggerPresent() ) \
						goto __CAT( L, __LINE__ ); \
					__debugbreak(); \
				} \
			} while(0)
	#else
		extern "C" int printf(const char *, ...);

		#define DebuggerBreak() asm("int3")

		#define Assert( x ) \
			do { \
				if ( !(x) ) \
				{ \
					::printf("Assertion failed %s:%d: %s\n", __FILE__, __LINE__, #x); \
					DebuggerBreak(); \
				} \
			} while(0)

		#define AssertMsg( x, msg ) \
			do { \
				if ( !(x) ) \
				{ \
					::printf("Assertion failed %s:%d: %s\n", __FILE__, __LINE__, msg); \
					DebuggerBreak(); \
				} \
			} while(0)

		#define AssertMsg1( x, msg, a1 ) \
			do { \
				if ( !(x) ) \
				{ \
					::printf("Assertion failed %s:%d: ", __FILE__, __LINE__); \
					::printf(msg, a1); \
					::printf("\n"); \
					DebuggerBreak(); \
				} \
			} while(0)

		#define AssertMsg2( x, msg, a1, a2 ) \
			do { \
				if ( !(x) ) \
				{ \
					::printf("Assertion failed %s:%d: ", __FILE__, __LINE__); \
					::printf(msg, a1, a2); \
					::printf("\n"); \
					DebuggerBreak(); \
				} \
			} while(0)
	#endif
	#define Verify( x ) Assert(x)
#else
	#define DebuggerBreak() ((void)0)
	#define Assert( x ) ((void)0)
	#define AssertMsg( x, msg ) ((void)0)
	#define AssertMsg1( x, msg, a1 ) ((void)0)
	#define AssertMsg2( x, msg, a1, a2 ) ((void)0)
	#define Verify( x ) x
#endif // _DEBUG

#ifdef _WIN32
	#define UNREACHABLE() do { Assert(!"UNREACHABLE"); __assume(0); } while(0)
#else
	#define UNREACHABLE() do { Assert(!"UNREACHABLE"); __builtin_unreachable(); } while(0)
#endif

#endif // SQDBG_DEBUG_H
