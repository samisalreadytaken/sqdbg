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
local dap = require("dap")

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

Specifier  | Format       | Example value             | Result
-----------|--------------|---------------------------|--------------
`x`        | hexadecimal  | 221                       | 0xdd
`X`        | hexadecimal  | 221                       | 0xDD
`xb`       | hexadecimal  | 221                       | dd
`Xb`       | hexadecimal  | 221                       | DD
`x0`       | hexadecimal  | 221                       | 0x000000dd
`x0b`      | hexadecimal  | 221                       | 000000dd
`X0`       | hexadecimal  | 221                       | 0x000000DD
`X0b`      | hexadecimal  | 221                       | 000000DD
`b`        | binary       | 221                       | 0b11011101
`bb`       | binary       | 221                       | 11011101
`b0`       | binary       | 221                       | 0b00000000000000000000000011011101
`b0b`      | binary       | 221                       | 00000000000000000000000011011101
`o`        | octal        | 221                       | 0335
`d`        | decimal      | 0xdd                      | 221
`c`        | character    | 0x41                      | 65 'A'
`f`        | float        | FLT\_MAX                  | 340282346638528859811704183484516925440.000000
`e`        | scientific   | 1250000.0                 | 1.250000e+06
`g`        | flt./sci.    | 1250000.0                 | 1.25e+06
`na`       | no address   | 0x010C5F20 {x = 0, y = 1} | x = 0, y = 1
`l`        | list members | 0x010C5F20 {size=4}       | 0x010C5F20 [26500, 29358, 15724, 26962]
`lna`      | list members | 0x010C5F20 {size=4}       | [26500, 29358, 15724, 26962]
`lnax`     | list members | 0x010C5F20 {size=4}       | [0x6784, 0x72ae, 0x3d6c, 0x6952]

### Watch scope locks

Flagging a watch expression 'locked' (`*` after a comma) will maintain the scope and executing thread of the expression at the time of its successful evaluation. Stepping preserves this lock while continuing execution clears it.

### Extended watch expressions

Binary literals with logical, arithmetic and bitwise operators are usable within watch expressions.

`~( 0b0101 >> ( -rand() & 012 ) ) - -0xf,b : 0b00001101`

Available special keywords: `$function`, `$caller`, `$stack`

### Function breakpoints

Use the syntax `funcname,filename:line` to set breakpoint on function _funcname_ found in file _filename_ at line _line_. `filename` and `line` are optional settings.

Use the function name `()` to set breakpoints on anonymous functions.

Line number is the line where the first instruction in a function is defined, or the opening bracket of the function if it was compiled without debuginfo. The breakpoints representing the functions below would be `constructor,script.nut:20` and `(),script.nut:26` (with debuginfo), or `constructor,script.nut:18` and `(),script.nut:24` (without debuginfo) respectively.

```
 16
 17 function CTest::constructor()
 18 {
 19
 20     local a = 1;
 21 }
 22
 23 local fn = function()
 24 {
 25
 26     dummy();
 27 }
 28
```

### Tracepoint / Logpoint

Expressions and format specifiers within `{}` are evaluated. Escape the opening bracket to print brackets `\{`.

Available special keywords: `$FUNCTION`, `$CALLER`, `$HITCOUNT`

### Class definitions

The script function `sqdbg_define_class` is used to display the class name and class instance values in variable views. Data breakpoints can be added on specified custom members.

```c
sqdbg_define_class( class, params )
```

Parameter      | Type                   | Description
---------------|------------------------|--------------
name           | string                 | Class name
value          | function->string       | Class instance value. Instance is passed to the function (`this` is the instance). Returns a string to be displayed in variable views. Instances of classes inherited from this class use this value unless they have their own values defined.
metamembers    | array                  | Elements of this array are passed to `_get` and `_set` metamethods of the class and displayed as instance members
custommembers  | array\|function->array | Elements of this array are tables that contain `string name`, `function get(optional key)`, `optional function set(optional key, val)` keys. This is useful for peeking into third-party or native classes that do not have exposed members. The functions are called within the instance environment.

Meta members example:

```js
class ExampleClass
{
	field = 1;

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

Custom members example on native classes with no Squirrel member variables:

```js
sqdbg_define_class( KeyValues,
{
	name = "KeyValues",
	value = KeyValues.GetName,
	// Populate unique member lists for each instance
	custommembers = function()
	{
		local ret = [];

		for ( local child = GetFirstSubKey(); child; child = child.GetNextKey() )
		{
			local kv = child;
			ret.append( {
				name = kv.GetName(),
				get = function()
				{
					if ( kv.GetFirstSubKey() )
						return kv;
					return kv.GetString();
				}
			} );
		}

		return ret;
	}
} );

// External accessors
local GetNetPropInt = function( key ) { return ::NetProps.GetPropInt( this, key ); }
local SetNetPropInt = function( key, val ) { return ::NetProps.SetPropInt( this, key, val ); }
local GetNetPropVector = function( key ) { return ::NetProps.GetPropVector( this, key ); }
local SetNetPropVector = function( key, val ) { return ::NetProps.SetPropVector( this, key, val ); }

sqdbg_define_class( C_BaseEntity,
{
	name = "C_BaseEntity",
	value = C_BaseEntity.tostring,
	custommembers =
	[
		{ name = "m_iHealth", get = GetNetPropInt, set = SetNetPropInt },
		{ name = "m_vecNetworkOrigin", get = GetNetPropVector, set = SetNetPropVector },
		{ name = "m_angNetworkAngles", get = GetNetPropVector, set = SetNetPropVector },
	]
} );

sqdbg_define_class( IPhysicsObject,
{
	name = "IPhysicsObject",
	value = IPhysicsObject.GetName,
	custommembers =
	[
		{ name = "inertia", get = IPhysicsObject.GetInertia },
		{ name = "isAsleep", get = IPhysicsObject.IsAsleep },
		{ name = "isCollisionEnabled", get = IPhysicsObject.IsCollisionEnabled },
		{ name = "isGravityEnabled", get = IPhysicsObject.IsGravityEnabled },
		{ name = "isMotionEnabled", get = IPhysicsObject.IsMotionEnabled },
		{ name = "mass", get = IPhysicsObject.GetMass, set = IPhysicsObject.SetMass },
	]
} );
```

### Function disassembly

The script function `sqdbg_disassemble` can be used to get information and disassembly of script functions.

Example:

```lua
function IntersectRayWithPlane( org, dir, normal, dist )
{
	local d	= dir.Dot( normal );
	if ( d )
		return ( dist - org.Dot( normal ) ) / d;
	return 0.0;
}

local out = sqdbg_disassemble( IntersectRayWithPlane );
foreach ( line in split(out, "\n") )
	print( line );
```

Output:

```js
stacksize     9
instructions  13
literals      1
localvarinfos 6
parameters    5
------
this, org, dir, normal, dist
------
0      0x08 5 0 2 6                 PREPCALLK [5] = [dir]->"Dot"
1      0x0A 7 3 0 0                 MOVE [7] = [normal]
2      0x06 5 5 6 2                 CALL [d] = [5] 2
3      0x1E 5 6 0 0                 JZ [d] 6
4      0x08 6 0 1 7                 PREPCALLK [6] = [org]->"Dot"
5      0x0A 8 3 0 0                 MOVE [8] = [normal]
6      0x06 6 6 7 2                 CALL [6] = [6] 2
7      0x12 6 6 4 0                 SUB [6] = [dist] - [6]
8      0x14 6 5 6 0                 DIV [6] /= [d]
9      0x17 1 6 7 0                 RETURN [6]
10     0x03 6 0 0 0                 LOADFLOAT [6] = 0
11     0x17 1 6 7 0                 RETURN [6]
12     0x17 255 0 0 0               RETURN
```

### Programmatic breakpoints

Script function         | Description
------------------------|--------------
`sqdbg_break`           | Break execution if a client is connected
`sqdbg_watch`           | Add data breakpoint on expression with optional condition and hit count

### Profiler

Script function         | Description
------------------------|--------------
`sqdbg_prof_start`      | Enable profiler and start collecting data
`sqdbg_prof_stop`       | Disable profiler and remove all collected data
`sqdbg_prof_pause`      | Pause profiler
`sqdbg_prof_resume`     | Resume paused profiler. Should be placed in the same call frame as `pause`
`sqdbg_prof_begin`      | Begin timing named block
`sqdbg_prof_end`        | End timing block. Should be placed in the same call frame as `begin`
`sqdbg_prof_reset`      | Reset profile data for all collected data or specified group in optionally specified thread
`sqdbg_prof_gets`       | Get profile report of the current or specified thread, or the specified block as string. Parameter optionally takes a thread, and requires group name or report type (0: call graph, 1: flat). E.g.: `sqdbg_prof_gets(1)` or `sqdbg_prof_gets(thread, 1)`. Measured peak times are ignored in total and average times in block reports.
`sqdbg_prof_print`      | Print profile report. Identical to printing each line from `sqdbg_prof_gets`

Example call graph output:
```
   %   total time  time/call      calls  func
 79.44    2.53  s  172.21 us      14690  FrameThink, keyframes.nut:2056 (0x437DD438)
 54.27    1.73  s  155.81 us      11091  |  ManipulatorThink, keyframes.nut:2700 (0x43736008)
 18.36  584.75 ms   45.89 us      12743  |  |  DrawCircleHalfBright, keyframes.nut:2444 (0x0CFAC200)
 17.22  548.43 ms   43.84 us      12510  |  |  DrawCircle, keyframes.nut:2421 (0x0C44E400)
  1.42   45.06 ms    2.36 us      19104  |  |  DrawRectFilled, keyframes.nut:2368 (0x0BB4BB00)
  1.23   39.17 ms    5.04 us       7776  |  |  Manipulator_DrawPlane, keyframes.nut:2627 (0x0D3C5800)
  0.41   13.07 ms    1.68 us       7776  |  |  |  VectorAngles, vs_math.nut:663 (0x0C44A000)
  1.12   35.72 ms  735.16 ns      48589  |  |  MatrixGetColumn, vs_math.nut:2965 (0x0AB7F800)
  0.96   30.56 ms    3.93 us       7776  |  |  Manipulator_DrawAxis, keyframes.nut:2591 (0x0CFAC900)
  0.48   15.16 ms    1.95 us       7776  |  |  |  VectorAngles, vs_math.nut:663 (0x0C44A000)
  0.94   30.01 ms    3.43 us       8761  |  |  VectorVectors, vs_math.nut:587 (0x0C449C00)
  0.90   28.81 ms    7.28 us       3956  |  |  Manipulator_IsIntersectingAxis, keyframes.nut:2560 (0x0C44EC00)
  0.65   20.56 ms    5.20 us       3956  |  |  |  IntersectRayWithRay, vs_math.nut:5980 (0x0CBBA200)
  0.21    6.81 ms  861.21 ns       7912  |  |  |  |  VectorNegate, vs_math.nut:952 (0x09B12980)
  0.57   18.00 ms   50.43 us        357  |  |  DrawGrid, keyframes.nut:2318 (0x0C0ABA00)
  0.54   17.04 ms    2.21 us       7716  |  |  IsRayIntersectingSphere, vs_math.nut:5736 (0x0C0C1F00)
  0.51   16.39 ms    5.68 us       2887  |  |  UpdateFromMatrix, keyframes.nut:826 (0x09B14780)
  0.22    6.97 ms    2.41 us       2887  |  |  |  MatrixAngles, vs_math.nut:1589 (0x0CBB6600)
  0.12    3.86 ms    1.34 us       2887  |  |  |  MatrixVectors, vs_math.nut:1562 (0x0C0C4580)
  0.44   14.16 ms    4.57 us       3102  |  |  DrawRectRotated, keyframes.nut:2379 (0x0C0AB680)
  0.34   10.73 ms    2.29 us       4686  |  |  IsRayIntersectingCircleSliceFront, keyframes.nut:2520 (0x0C0AB300)
  0.31    9.90 ms    3.43 us       2887  |  |  MatrixBuildRotationAboutAxis, vs_math.nut:3318 (0x0CFA6700)
  0.28    8.92 ms    1.41 us       6315  |  |  IntersectInfiniteRayWithSphere, vs_math.nut:5778 (0x0C85DA00)
  0.27    8.67 ms    2.59 us       3352  |  |  Manipulator_IsIntersectingPlane, keyframes.nut:2580 (0x0B708EC0)
  0.04    1.40 ms  417.75 ns       3352  |  |  |  IntersectRayWithPlane, vs_math.nut:6013 (0x0A711480)
 19.40  617.70 ms   55.69 us      11091  |  DrawFrustum, keyframes.nut:861 (0x43768748)
  6.80  216.63 ms   19.53 us      11091  |  |  MatrixInverseGeneral, vs_math.nut:2823 (0x436E9388)
  3.99  127.20 ms    1.43 us      88728  |  |  Vector3DMultiplyPositionProjective, vs_math.nut:3480 (0x0C862F00)
  3.67  116.91 ms   10.54 us      11091  |  |  WorldToScreenMatrix, vs_math.nut:3861 (0x0CFA6E00)
...
 13.83  440.50 ms  149.93 us       2938  EditModeThink, keyframes.nut:1661 (0x4366CBD8)
  8.63  274.69 ms   83.77 us       3279  |  DrawFrustum1, keyframes.nut:846 (0x0A711C00)
  8.48  269.94 ms   82.35 us       3278  |  |  DrawViewFrustum, vs_math.nut:4054 (0x0C0C3400)
  5.23  166.42 ms   50.77 us       3278  |  |  |  DrawFrustum, vs_math.nut:4033 (0x0CFA7500)
  4.83  153.90 ms    3.91 us      39336  |  |  |  |  0x0B707B80, vs_math.nut:3990
  3.60  114.52 ms    1.46 us      78672  |  |  |  |  |  Vector3DMultiplyPositionProjective, vs_math.nut:3480 (0x0C862F00)
  1.92   61.05 ms   18.62 us       3278  |  |  |  MatrixInverseGeneral, vs_math.nut:2823 (0x436E9388)
  1.01   32.21 ms    9.82 us       3278  |  |  |  WorldToScreenMatrix, vs_math.nut:3861 (0x0CFA6E00)
...
```

Example flat profile output:
```
   %   total time  time/call      calls  func
 28.17    2.53  s  172.21 us      14690  FrameThink, keyframes.nut:2056 (0x437DD438)
 19.24    1.73  s  155.81 us      11091  ManipulatorThink, keyframes.nut:2700 (0x43736008)
  6.88  617.70 ms   55.69 us      11091  DrawFrustum, keyframes.nut:861 (0x43768748)
  6.51  584.75 ms   45.89 us      12743  DrawCircleHalfBright, keyframes.nut:2444 (0x0CFAC200)
  6.11  548.43 ms   43.84 us      12510  DrawCircle, keyframes.nut:2421 (0x0C44E400)
  4.91  440.50 ms  149.93 us       2938  EditModeThink, keyframes.nut:1661 (0x4366CBD8)
  3.09  277.92 ms   19.32 us      14382  MatrixInverseGeneral, vs_math.nut:2823 (0x436E9388)
  3.06  274.69 ms   83.77 us       3279  DrawFrustum1, keyframes.nut:846 (0x0A711C00)
  3.01  270.03 ms   82.35 us       3279  DrawViewFrustum, vs_math.nut:4054 (0x0C0C3400)
  2.69  241.78 ms    1.44 us     167436  Vector3DMultiplyPositionProjective, vs_math.nut:3480 (0x0C862F00)
  1.85  166.47 ms   50.77 us       3279  DrawFrustum, vs_math.nut:4033 (0x0CFA7500)
  1.71  153.95 ms    3.91 us      39348  0x0B707B80, vs_math.nut:3990
  1.66  149.24 ms   10.38 us      14382  WorldToScreenMatrix, vs_math.nut:3861 (0x0CFA6E00)
  0.67   60.56 ms    2.71 us      22373  DrawRectRotated, keyframes.nut:2379 (0x0C0AB680)
  0.58   52.06 ms    3.79 us      13746  MainViewOrigin, keyframes.nut:644 (0x0AB80000)
  0.50   45.06 ms    2.36 us      19104  DrawRectFilled, keyframes.nut:2368 (0x0BB4BB00)
  0.48   43.39 ms    3.02 us      14382  MatrixMultiply, vs_math.nut:3215 (0x436D9B58)
  0.44   39.17 ms    5.04 us       7776  Manipulator_DrawPlane, keyframes.nut:2627 (0x0D3C5800)
  0.40   35.72 ms  735.16 ns      48589  MatrixGetColumn, vs_math.nut:2965 (0x0AB7F800)
  0.36   32.02 ms    2.23 us      14382  ComputeCameraVariables, vs_math.nut:3835 (0x0C85EE00)
  0.34   30.56 ms    3.93 us       7776  Manipulator_DrawAxis, keyframes.nut:2591 (0x0CFAC900)
  0.33   30.01 ms    3.43 us       8761  VectorVectors, vs_math.nut:587 (0x0C449C00)
  0.32   28.81 ms    7.28 us       3956  Manipulator_IsIntersectingAxis, keyframes.nut:2560 (0x0C44EC00)
  0.31   28.23 ms    1.82 us      15552  VectorAngles, vs_math.nut:663 (0x0C44A000)
  0.23   21.06 ms  488.17 ns      43146  constructor, vs_math.nut:423 (0x0C0C5A80)
  0.23   20.56 ms    5.20 us       3956  IntersectRayWithRay, vs_math.nut:5980 (0x0CBBA200)
  0.20   18.00 ms   50.43 us        357  DrawGrid, keyframes.nut:2318 (0x0C0ABA00)
...
```

Example block profile output:
```
(sqdbg) prof | CSGOHudWeaponSelection::Paint                   : total 285.39 ms, avg 109.55 us, peak  66.59 ms(1), hits 2606
(sqdbg) prof | CSGOHudWeaponSelection::GetWeapon               : total  28.66 ms, avg   9.95 us, peak  43.10 us(1944), hits 2880
(sqdbg) prof | CSGOHudWeaponSelection::PerformLayoutInternal   : total  49.74 ms, avg 417.96 us, peak  15.69 ms(1), hits 120
```

## Notes

### Line breakpoints

Line breakpoints, data breakpoint locations and accurate line stepping require scripts to be compiled with debug information available. The debugger enables this on attachment, however this may be after some scripts are loaded. `enabledebuginfo(1)` script function can be used before loading scripts to ensure that they are compiled with debug information.

Without this debug info, you may still break with `sqdbg_break`, set function and exception breakpoints, and step execution.

### Breaking execution

When a function or exception breakpoint is hit, the debugger cannot determine which file the break occured in because Squirrel is only aware of the "source name" passed in by the parent program. Adding a line breakpoint in a file in your editor registers its name with its path in the debugger. If multiple files with the same name exist, the path of the file with the most recent breakpoint will be assumed as the script path.

If source files are unavailable, you may always use the disassembly view.

### Function breakpoints

Adding named function breakpoints require the desired functions to be compiled in the syntax `function MyFunc()` instead of `MyFunc <- function()`. In Squirrel, the former sets the name of the function while the latter creates a nameless, anonymous function which can be broken into by specifying file name and line number in the anonymous function breakpoint.

### Special accessors

Use the keywords `__this`, `__vargv`, `__vargc` in REPL and breakpoint conditions to access current environment and the local vargv respectively. Using `this` and `vargv` in watch and tracepoint expressions will work fine.

### Data breakpoints

Local variable watches are automatically removed at the end of the scope of the target variable.

Condition requires a token as prefix. Strict (in)equality requires matching type (i.e. float is not equal to integer). Multiple breakpoints with different conditions can be added on a single variable to match multiple values.

The condition is compiled at the time and within the stack frame of its creation. Late lookups are not supported.

Token | Description                     | Example input   | Evaluation
------|---------------------------------|-----------------|------------------
`==`  | equal (strict)                  | `== this`       | `data == {table}`
`!=`  | not equal (strict)              | `!= 0`          | `data != 0`
`>`   | greater than                    | `> PI * 0.5`    | `data > 1.570796`
`>=`  | greater than or equal to        | `>= 0`          | `data >= 0`
`<`   | less than                       | `< rand()`      | `data < 18467`
`<=`  | less than or equal to           | `<= 0`          | `data <= 0`
`&`   | bitwise AND, not equal to zero  | `& 0x2`         | `(data & 0x2) != 0`
`!&`  | bitwise AND, equal to zero      | `!& 0x2`        | `(data & 0x2) == 0`
`=&`  | bitwise AND, equal to input     | `=& 0x2 \| 0x4` | `(data & 0x6) == 0x6`

## Licence

MIT, see [LICENSE](LICENSE).
