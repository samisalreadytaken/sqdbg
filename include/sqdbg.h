//-----------------------------------------------------------------------
//                       github.com/samisalreadytaken/sqdbg
//-----------------------------------------------------------------------
//
// Squirrel Debugger
//

#ifndef SQDBG_SERVER_H
#define SQDBG_SERVER_H

#include <squirrel.h>

#define SQDBG_SV_API_VER 1

struct SQDebugServer;
typedef SQDebugServer* HSQDEBUGSERVER;

#ifdef __cplusplus
extern "C" {
#endif

// Create and attach a new debugger
// Memory is owned by the VM, it is freed when the VM dies or
// the debugger is disconnected via sqdbg_destroy_debugger()
extern HSQDEBUGSERVER sqdbg_attach_debugger( HSQUIRRELVM vm );

// Detach and destroy the debugger attached to this VM
// Invalidates the handle returned from sqdbg_attach_debugger()
extern void sqdbg_destroy_debugger( HSQUIRRELVM vm );

// Open specified port and allow client connections
// Returns 0 on success
extern int sqdbg_listen_socket( HSQDEBUGSERVER dbg, unsigned short port );

// Process client connections and incoming messages
extern void sqdbg_frame( HSQDEBUGSERVER dbg );

// Redirects to sq_compilebuffer
// If the VM has a debugger attached, copies every script to be able to source them to debugger clients
extern SQRESULT sqdbg_compilebuffer( HSQUIRRELVM vm, const SQChar *script, SQInteger size,
		const SQChar *sourcename, SQBool raiseerror );

#ifdef __cplusplus
}
#endif

#endif // SQDBG_SERVER_H
