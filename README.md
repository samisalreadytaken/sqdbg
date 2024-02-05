# Squirrel Debugger

Remote debugger for [Squirrel Language](https://github.com/albertodemichelis/squirrel) using the [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol).

Supports Squirrel 2.1.2 and later.

## Integration

The debugger lives alongside Squirrel VM, it does not require modified Squirrel code. See [sqdbg.h](include/sqdbg.h) for available API.

Minimal example:

```c
#include <squirrel.h>
#include <sqdbg.h>

void Sleep( int ms );
void printfunc( HSQUIRRELVM vm, const SQChar *s, ... );

int main()
{
	HSQUIRRELVM vm = sq_open( 1024 );
	sq_setprintfunc( vm, printfunc, printfunc );

	HSQDEBUGSERVER dbg = sqdbg_attach_debugger( vm );
	sqdbg_listen_socket( dbg, 2222 );

	for (;;)
	{
		// Needs to be called frequently
		// to process connections and messages
		sqdbg_frame( dbg );
		Sleep( 20 );
	}

	sq_close( vm );

	return 0;
}
```

## Usage (client)

Refer to your client manual on attaching to a remote port.

### vimspector

<details><summary>vimrc config</summary>

```vim
	let g:vimspector_adapters =
\	{
\		"squirrel": {
\			"name": "squirrel",
\			"command": "attach",
\			"port": "${port}",
\			"host": "${host:127.0.0.1}",
\			"configuration": {
\				"request": "attach"
\			}
\		}
\	}
	let g:vimspector_configurations =
\	{
\		"Squirrel attach": {
\			"adapter": "squirrel",
\			"filetypes": [ "squirrel" ],
\			"configuration": {
\				"request": "attach"
\			},
\			"breakpoints": {
\				"exception": {
\					"unhandled": "Y",
\					"all": "N"
\				}
\			}
\		}
\	}
```

</details>

### nvim-dap

<details><summary>vimrc config</summary>

```lua
local dap = require('dap')

dap.adapters.squirrel = function( callback, config )
	callback( {
		type = "server",
		host = vim.fn.input("Enter debug server address: ", "127.0.0.1"),
		port = vim.fn.input("Enter debug server port: ")
	} )
end

dap.configurations.squirrel =
{
	{
		name = "Attach",
		type = "squirrel",
		request = "attach"
	}
}
```

</details>

### VS Code

This editor requires extensions to recognise the existence of debuggers. Install the following extension for Squirrel debugger compatibility: [github.com/samisalreadytaken/sqdbg-vs](https://github.com/samisalreadytaken/sqdbg-vs)

## Additional features

Debugger specific features that are outside of the _Debug Adapter Protocol_.

### Format specifiers

Append a flag on a watch or tracepoint expression after a comma (`,`) to format the variable.

Available flags: `x` (hexadecimal), `b` (binary).

### Watch scope locks

Flagging a watch variable 'locked' (`*` after a comma) will maintain the scope and executing thread of the expression at the time of its successful evaluation. Stepping preserves this lock while continuing execution clears it.

### Special watch expressions

`$function`, `$caller`

### Function breakpoints

Use the syntax `funcname,filename` to set breakpoints on function _funcname_ found in file _filename_.

Use the function name `()` to set breakpoints on anonymous functions.

### Tracepoint / Logpoint

Expressions and format specifiers within `{}` are evaluated. Escape the opening bracket to print brackets `\{`.

Available special keywords: `$FUNCTION`, `$CALLER`, `$HITCOUNT`

### Class definitions

The script function `sqdbg_define_class` is used to display the class name and class instance values in variable views.

```c
sqdbg_define_class( class, params )
```

Parameter      | Type             | Description
---------------|------------------|--------------
name           | string           | Class name
value          | function->string | Class instance value. Instance is passed to the function (`this` is the instance). Returns a string to be displayed in variable views. Instances of classes inherited from this class use this value unless they have their own values defined.
metamembers    | array            | Elements of this array are passed to `_get` and `_set` metamethods of the class and displayed as instance members

Example:

```js
class ExampleClass
{
	field = 24;

	function _get(i)
	{
		if ( i == "random" )
			return ::rand();

		throw "the index '"+i+"' does not exist";
	}
}

sqdbg_define_class( ExampleClass,
{
	name = "ExampleClass",
	value = function() { return ::format( "field = %d, random = %d", field, random ); }
	metamembers = [ "random" ]
} );
```

```lua
local test = ExampleClass();
test.field = 33;
```

Inspecting the local variable `test` above will then show the following values:

```lua
v test: 0x000001DD43DFE100 {field = 33, random = 15724}
    $refs: 1
  > $class: 0x000001DD43E00980 ExampleClass
    random: 18467
    field: 33
```

### Function disassembly

The script function `sqdbg_disassemble` can be used to get information and disassembly of input functions.

Example:

```lua
function test()
{
	local arr = array(4);
	foreach ( i, v in arr )
		arr[i] = rand();
	print(arr + "\n");
}

local out = sqdbg_disassemble( test );
print( out );
```

Output:

```js
stacksize     7
instructions  15
literals      4
localvarinfos 5
------
0      0x08 1 0 0 2                 PREPCALLK "array"
1      0x02 3 4 0 0                 LOADINT 4
2      0x06 1 1 2 2                 CALL
3      0x18 2 3 0 0                 LOADNULLS 3
4      0x33 1 6 2 0                 FOREACH
5      0x34 1 6 2 0                 POSTFOREACH
6      0x08 5 1 0 6                 PREPCALLK "rand"
7      0x06 5 5 6 1                 CALL
8      0x0D 255 1 2 5               SET
9      0x1C 0 -6 0 0                JMP -6
10     0x08 2 2 0 3                 PREPCALLK "print"
11     0x01 4 3 0 0                 LOAD "\n"
12     0x11 4 4 1 0                 ADD
13     0x06 255 2 3 2               CALL
14     0x17 255 0 0 0               RETURN
```

### Manual breakpoint

The script function `sqdbg_break` can be used to break execution while a debugger is attached. This can be used to implement assertions instead of throwing exceptions as exceptions are not recoverable.

## Notes

### Stepping and line breakpoints

Stepping, line breakpoints, and data breakpoint locations require scripts to be compiled with debug information available. The debugger enables this when attaching, however this may be after some scripts are loaded. You may use `enabledebuginfo(1)` script function before loading your scripts to ensure that they are compiled with debug information.

Without this debug info, you may still set function and exception breakpoints, and step out of function breakpoints, but not step in.

### Breaking execution (source files)

When a function or exception breakpoint is hit, the debugger cannot determine which file the break occured in because Squirrel is only aware of the "source name" passed in by the parent program. Adding a breakpoint in a file in your editor registers its name with its path in the debugger. If multiple files with the same name exist, the path of the file with the most recent breakpoint will be assumed as the script path.

If source files are unavailable, you may always use the disassembly view.

### Function breakpoints

To be able to set function breakpoints, the functions you want to break into need to have been compiled in the syntax `function MyFunc()` instead of `MyFunc <- function()`. In Squirrel, the former sets the name of the function while the latter creates a nameless, anonymous function.

### Watch variables

If you wish to watch a variable in the current environment and a variable with the same name exists as a local variable, use the keyword `__this` to access it (e.g. `__this.myvar`).

### Data breakpoint conditions

The condition is directly checked against the value of the data (as in `value == condition`).

## Licence

MIT, see [LICENSE](LICENSE).
